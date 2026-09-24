// 还原任务暂存实现。
#include "task.h"

#include <windows.h>

#include <cstdio>
#include <string>

#include "../common/version.h"

namespace sysrecover {
namespace {

std::string W2U(const std::wstring& w) {
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0,
                                nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, 0);
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr,
                            nullptr);
    return s;
}

// JSON 字符串转义（反斜杠/引号），供 json 侧写 Windows 路径。
std::string JsonEsc(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\\' || c == '"')
            out += '\\';
        out += c;
    }
    return out;
}

bool WriteTextFile(const std::wstring& path, const std::string& utf8) {
    HANDLE h =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    BOOL ok = ::WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written,
                          nullptr);
    CloseHandle(h);
    return ok && written == utf8.size();
}

}  // namespace

// 任务 sidecar（key=value，Linux restore.sh 读取）。软件目录与目标分区根
// 各写一份：日志写目标根、conf 写软件目录，二者可能不在同一分区，Linux 侧
// 需能就近取到（PIT-035）。
std::string BuildTaskConf(const RestoreTask& t) {
    char buf[8192];
    snprintf(
        buf, sizeof(buf),
        "action=restore\n"
        "pt_type=%s\n"
        "image_part_guid=%s\n"
        "image_rel_path=%s\n"
        "image_path=%s\n"
        "image_index=%d\n"
        "target_guid=%s\n"
        "target_offset=%llu\n"
        "target_size=%llu\n"
        "target_disk_serial=%s\n"
        "repair_boot=%d\n"
        "partition_count=%u\n"
        "software_dir=%s\n",
        t.ptType.c_str(), W2U(t.imagePartGuid).c_str(),
        W2U(t.imageRelPath).c_str(), W2U(t.imagePath).c_str(), t.imageIndex,
        W2U(t.targetGuid).c_str(),
        (unsigned long long)t.targetOffset,
        (unsigned long long)t.targetSize, W2U(t.targetDiskSerial).c_str(),
        t.repairBoot ? 1 : 0, t.partCount, W2U(t.softwareDir).c_str());
    return buf;
}

std::string BuildTaskJson(const RestoreTask& t) {
    char buf[8192];
    snprintf(
        buf, sizeof(buf),
        "{\"schema\":1,\"action\":\"restore\",\"pt_type\":\"%s\","
        "\"image_path\":\"%s\",\"image_rel_path\":\"%s\",\"image_index\":%d,"
        "\"target_disk\":%d,\"target_part\":%d,"
        "\"target_offset\":%llu,\"target_size\":%llu,"
        "\"repair_boot\":%s,\"partition_count\":%u}\n",
        t.ptType.c_str(), JsonEsc(W2U(t.imagePath)).c_str(),
        W2U(t.imageRelPath).c_str(), t.imageIndex,
        t.targetDisk, t.targetPart, (unsigned long long)t.targetOffset,
        (unsigned long long)t.targetSize, t.repairBoot ? "true" : "false",
        t.partCount);
    return buf;
}

std::string BuildRestoreLogText(const RestoreTask& t,
                                const std::string& isoTimestamp) {
    char buf[8192];
    snprintf(buf, sizeof(buf),
             "action=restore\n"
             "log_time=%s\n"
             "software_version=%s\n"
             "software_path=%s\n"
             "target_disk_name=%s\n"
             "target_disk_serial=%s\n"
             "target_disk_size=%llu\n"
             "target_part_offset=%llu\n"
             "target_part_size=%llu\n"
             "target_fs=%s\n"
             "target_vol_label=%s\n"
             "image_path=%s\n"
             "image_index=%d\n"
             "repair_boot=%d\n"
             "pt_type=%s\n",
             isoTimestamp.c_str(), SYSRECOVER_VERSION,
             W2U(t.softwarePath).c_str(),
             W2U(t.targetDiskName).c_str(), W2U(t.targetDiskSerial).c_str(),
             (unsigned long long)t.targetDiskSize,
             (unsigned long long)t.targetOffset,
             (unsigned long long)t.targetSize, W2U(t.targetFs).c_str(),
             W2U(t.targetVolLabel).c_str(), W2U(t.imagePath).c_str(),
             t.imageIndex, t.repairBoot ? 1 : 0, t.ptType.c_str());
    return buf;
}

bool WriteRestoreTask(const std::wstring& dir, const RestoreTask& t,
                      std::string& log) {
    // sidecar（key=value，供 Linux restore.sh）
    // 注意：image_path 为 restore.sh 第 7 步定位镜像分区的必需键（Phase 5 补）。
    if (!WriteTextFile(dir + L"\\restore-task.conf", BuildTaskConf(t))) {
        log += "write restore-task.conf FAIL\n";
        return false;
    }
    // json（同内容，供 Windows 侧/GUI 读取）
    if (!WriteTextFile(dir + L"\\restore-task.json", BuildTaskJson(t))) {
        log += "write restore-task.json FAIL\n";
        return false;
    }
    log += "restore-task written\n";
    return true;
}

bool WriteRestoreLog(wchar_t letter, const RestoreTask& t, std::string& log) {
    wchar_t root[4] = {letter, L':', L'\\', 0};
    // 1) 清除旧 _zjresy*.log（对齐旧 C# ClearOldRestoreLogs）
    WIN32_FIND_DATAW fd;
    std::wstring pattern = std::wstring(root) + L"_zjresy*.log";
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            DeleteFileW((std::wstring(root) + fd.cFileName).c_str());
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
    // 2) 写新日志（文件名含日时分，对齐旧 C# 命名）
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf(name, 64, L"_zjresy%02u%02u%02u.log", st.wDay, st.wHour,
             st.wMinute);
    char timeBuf[40];
    snprintf(timeBuf, sizeof(timeBuf), "%04u-%02u-%02uT%02u:%02u:%02u",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    if (!WriteTextFile(std::wstring(root) + name,
                       BuildRestoreLogText(t, timeBuf))) {
        log += "write _zjresy log FAIL\n";
        return false;
    }
    // 同分区根并写一份 conf：Linux 侧在日志所在分区即可就近取到，避免
    // 「日志在 C:、conf 在 D:\软件目录」导致 S99zjrestore 找不到任务（PIT-035）。
    WriteTextFile(std::wstring(root) + L"restore-task.conf", BuildTaskConf(t));
    log += "restore log written to ";
    log += W2U(std::wstring(root) + name);
    log += "\n";
    return true;
}

}  // namespace sysrecover
