#include "app/imgsrc.h"

#include "common/i18n.h"
#include "common/logger.h"

#include <windows.h>

#include <map>
#include <mutex>

namespace sysrecover {
namespace {

std::mutex g_mu;
std::map<std::wstring, std::wstring> g_passwords;  // path → 口令（仅内存）
std::map<std::wstring, std::wstring> g_temps;      // .zje 路径 → 临时明文

std::wstring Lower(const std::wstring& s) {
    std::wstring r = s;
    for (auto& c : r)
        c = (wchar_t)towlower(c);
    return r;
}

std::wstring PasswordFor(const std::wstring& path) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_passwords.find(Lower(path));
    return it == g_passwords.end() ? std::wstring() : it->second;
}

}  // namespace

void SetImagePassword(const std::wstring& path, const std::wstring& pwd) {
    std::lock_guard<std::mutex> lk(g_mu);
    const std::wstring k = Lower(path);
    if (pwd.empty())
        g_passwords.erase(k);
    else
        g_passwords[k] = pwd;
}

std::wstring GetImagePassword(const std::wstring& path) {
    return PasswordFor(path);
}

void ClearImagePasswords() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_passwords.clear();
}

bool ImageIsEncrypted(const std::wstring& path) {
    return ZjeIsFile(path);
}

int ImageProbe(const std::wstring& path, std::wstring& why) {
    why.clear();
    if (!ImageIsEncrypted(path)) {
        WimEngine eng;
        return eng.Probe(path, why);
    }
    const std::wstring pwd = PasswordFor(path);
    if (pwd.empty()) {
        why = Tr(L"这是加密镜像，需要先输入密码");
        return -1;
    }
    ZjeInfo info;
    std::vector<ZjeImage> imgs;
    std::wstring err;
    const int rc = ZjeReadMeta(path, pwd, &info, &imgs, err);
    if (rc != 0) {
        why = err;
        return -1;
    }
    if (imgs.empty()) {
        why = Tr(L"镜像里没有任何子镜像");
        return -1;
    }
    return 0;
}

int ImageList(const std::wstring& path, std::vector<ImageDesc>& out) {
    out.clear();
    if (!ImageIsEncrypted(path)) {
        WimEngine eng;
        return eng.ListImages(path, out);
    }
    const std::wstring pwd = PasswordFor(path);
    if (pwd.empty())
        return -1;
    ZjeInfo info;
    std::vector<ZjeImage> imgs;
    std::wstring err;
    if (ZjeReadMeta(path, pwd, &info, &imgs, err) != 0)
        return -1;
    for (const auto& z : imgs) {
        ImageDesc d;
        d.index = z.index;
        d.name = z.name;
        d.description = z.description;
        d.sizeBytes = z.sizeBytes;
        d.creationTime = z.creationTime;
        out.push_back(std::move(d));
    }
    return out.empty() ? -1 : 0;
}

int ImageContentSize(const std::wstring& path, int index,
                     unsigned long long* bytes) {
    if (!bytes)
        return -1;
    *bytes = 0;
    if (!ImageIsEncrypted(path)) {
        WimEngine eng;
        return eng.ImageSize(path, index, bytes);
    }
    const std::wstring pwd = PasswordFor(path);
    if (pwd.empty())
        return -1;
    ZjeInfo info;
    std::vector<ZjeImage> imgs;
    std::wstring err;
    if (ZjeReadMeta(path, pwd, &info, &imgs, err) != 0)
        return -1;
    for (const auto& z : imgs) {
        if (z.index == (index < 1 ? 1 : index)) {
            *bytes = z.sizeBytes;
            return 0;
        }
    }
    return -1;
}

int ImageHasWinload(const std::wstring& path, int index, bool* exists) {
    if (exists)
        *exists = false;
    if (!ImageIsEncrypted(path)) {
        WimEngine eng;
        return eng.ImagePathExists(path, index, L"\\Windows\\system32\\winload.exe",
                                   exists);
    }
    const std::wstring pwd = PasswordFor(path);
    if (pwd.empty())
        return -1;
    ZjeInfo info;
    std::vector<ZjeImage> imgs;
    std::wstring err;
    if (ZjeReadMeta(path, pwd, &info, &imgs, err) != 0)
        return -1;
    for (const auto& z : imgs) {
        if (z.index == (index < 1 ? 1 : index)) {
            if (exists)
                *exists = z.hasWinload;
            return 0;
        }
    }
    return -1;
}

std::wstring ResolveImagePlain(const std::wstring& path, ProgressFn progress,
                               std::wstring& err, bool* temp) {
    err.clear();
    if (temp)
        *temp = false;
    if (!ImageIsEncrypted(path))
        return path;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_temps.find(Lower(path));
        if (it != g_temps.end() &&
            GetFileAttributesW(it->second.c_str()) != INVALID_FILE_ATTRIBUTES)
            return it->second;
    }
    const std::wstring pwd = PasswordFor(path);
    if (pwd.empty()) {
        err = Tr(L"这是加密镜像，需要先输入密码");
        return {};
    }
    const std::wstring plain = ZjeTempPlainPath(path);
    CryptProgress cp = [&](unsigned long long done, unsigned long long total) {
        if (!progress)
            return false;
        int pct = total ? (int)(done * 100 / total) : -1;
        return progress(pct, "decrypt");
    };
    std::wstring derr;
    LogInfo("ZJE: decrypting image for read access");
    if (ZjeDecryptFile(path, plain, pwd, cp, derr) != 0) {
        err = derr;
        LogError("ZJE decrypt failed: " + std::string(derr.begin(), derr.end()));
        // 取消（err=已取消）也要把半成品清掉 —— ZjeDecryptFile 已删 .tmp
        return {};
    }
    if (temp)
        *temp = true;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_temps[Lower(path)] = plain;
    }
    return plain;
}

void ReleaseImagePlain(const std::wstring& path) {
    std::wstring tmp;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_temps.find(Lower(path));
        if (it == g_temps.end())
            return;
        tmp = it->second;
        g_temps.erase(it);
    }
    if (!tmp.empty())
        DeleteFileW(tmp.c_str());
}

void ReleaseAllImagePlain() {
    std::map<std::wstring, std::wstring> copy;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        copy.swap(g_temps);
    }
    for (const auto& kv : copy)
        DeleteFileW(kv.second.c_str());
}

}  // namespace sysrecover
