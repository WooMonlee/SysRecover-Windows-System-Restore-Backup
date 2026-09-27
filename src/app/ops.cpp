// 备份/还原共享操作层实现。语义对齐 CLI（Phase 2-4 已验证）与旧 C# 编排器。
#include "ops.h"

#include <windows.h>

#include <cstdio>
#include <cwctype>
#include <vector>

#include "../boot/bcd.h"
#include "../boot/bootfix.h"
#include "../boot/grub.h"
#include "../boot/task.h"
#include "../boot/uefi.h"
#include "../common/logger.h"
#include "../common/process.h"
#include "../common/progress.h"
#include "../common/singleton.h"
#include "../common/vss.h"
#include "../common/version.h"
#include "../disk/disk.h"
#include "../wim/exclude.h"
#include "safety.h"

namespace sysrecover {
namespace {

// 网络路径判定：UNC（\\server\share\...）或**映射网络驱动器**（DRIVE_REMOTE）。
// 用途：**暂存+重启**的还原由 Linux 救援层读镜像，而救援层**访问不到网络** →
// 网络镜像必然 image not found，必须在格式化之前就拦下（问题清单 D1/H1）。
bool IsNetworkPath(const std::wstring& p) {
    if (p.size() >= 2 && (p[0] == L'\\' || p[0] == L'/') &&
        (p[1] == L'\\' || p[1] == L'/'))
        return true;  // \\server\share\...
    if (p.size() >= 2 && p[1] == L':') {
        wchar_t root[4] = {p[0], L':', L'\\', 0};
        if (GetDriveTypeW(root) == DRIVE_REMOTE)
            return true;  // 映射网络驱动器 Z:\...
    }
    return false;
}

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

std::wstring ExePath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// 递归复制目录（软件自保用；不处理 ACL）。
bool CopyDir(const std::wstring& src, const std::wstring& dst) {
    if (!IsDir(src))
        return false;
    CreateDirectoryW(dst.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((src + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    bool ok = true;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        std::wstring s = src + L"\\" + fd.cFileName;
        std::wstring d = dst + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ok &= CopyDir(s, d);
        else if (!CopyFileW(s.c_str(), d.c_str(), FALSE))
            ok = false;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

bool SameDrive(wchar_t a, wchar_t b) {
    return towupper(a) == towupper(b);
}

// 就地还原：快速格式化 → wimlib 目录模式应用（PIT-008）→ 系统镜像才 bcdboot
// 修引导。**全程不重启**。
int RunDirectRestore(const RestoreRequest& req, const PartitionInfo& target,
                     const std::wstring& imagePath, std::string& err,
                     ProgressFn progress) {
    std::wstring letter(1, target.letter[0]);
    LogInfo("in-place restore -> " + W2U(letter) + ": (" + W2U(imagePath) +
            ", index=" + std::to_string(req.index) + ")");
    // 1) 快速格式化（不传 /V：保留原卷标）
    {
        std::string out;
        int rc = RunProcess(SysToolPath(L"format.com"),
                            letter + L": /FS:NTFS /Q /Y", out);
        LogInfo("format.com rc=" + std::to_string(rc) + " / " + out);
        if (rc != 0) {
            err = "就地还原：格式化目标分区失败（" + out + "）";
            LogError(err);
            ProgressDone("restore", "failed");
            return 1;
        }
    }
    // 2) 应用镜像（目录模式）
    WimEngine wim;
    if (!wim.ok()) {
        err = "wimlib 初始化失败";
        ProgressDone("restore", "failed");
        return 1;
    }
    int arc = wim.Apply(imagePath, req.index, letter + L":\\",
                        [progress](int pct, const std::string& st) {
                            ProgressUpdate("restore", pct, st);
                            // 转投给上层（GUI 用来刷新进度条/百分比）；返回 true = 中止。
                            if (progress)
                                return progress(pct, st);
                            return false;
                        });
    if (arc != 0) {
        err = "就地还原：应用镜像失败 rc=" + std::to_string(arc) + " (" +
              W2U(WimEngine::ErrorString(arc)) + ")";
        LogError(err);
        ProgressDone("restore", "failed");
        return 1;
    }
    // 3) 只有系统镜像才修引导（看目标里有没有 winload.exe）
    if (req.repairBoot) {
        std::wstring winDir = letter + L":\\Windows";
        if (GetFileAttributesW((winDir + L"\\System32\\winload.exe").c_str()) !=
            INVALID_FILE_ATTRIBUTES) {
            std::string out;
            if (IsUefiFirmware()) {
                PartitionInfo esp;
                std::string blog;
                if (FindEspPartition(esp)) {
                    std::wstring espRoot = MountEsp(blog);
                    if (!espRoot.empty()) {
                        int brc = RunProcess(
                            SysToolPath(L"bcdboot.exe"),
                            winDir + L" /s " + std::wstring(1, espRoot[0]) +
                                L": /f UEFI",
                            out);
                        LogInfo("bcdboot (UEFI) rc=" + std::to_string(brc) +
                                " / " + out);
                        UnmountEsp(espRoot, blog);
                    }
                }
            } else {
                int brc = RunProcess(SysToolPath(L"bcdboot.exe"),
                                     winDir + L" /s " + letter + L": /f BIOS",
                                     out);
                LogInfo("bcdboot (BIOS) rc=" + std::to_string(brc) + " / " +
                        out);
            }
        } else {
            LogInfo("in-place: no winload.exe -> not a system image, skip "
                    "bcdboot");
        }
    }
    ProgressDone("restore", "done");
    LogInfo("in-place restore done (no reboot needed)");
    return 0;
}

}  // namespace

// 目标分区是否**未**被占用（可以就地写）？—— 对外可见（ops.h），因为 GUI/CLI 要用它
// 决定提示文案（"需不需要重启"）；StageRestore 内部用的是同一个函数，不会漂移。
// 判据（用户规格）：不是正在运行的系统盘，且能对该卷加独占锁 ——
// FSCTL_LOCK_VOLUME 会因"卷上有打开的文件/句柄"而失败。
// 能就地写 → 直接还原（PE 里 / 还原到非系统盘）；否则走暂存 + 重启 + Linux。
bool CanRestoreInPlace(const PartitionInfo& t, std::string& why) {
    if (t.letter.empty()) {
        why = "目标分区没有盘符（无法就地写）";
        return false;
    }
    wchar_t winDir[MAX_PATH] = {};
    if (GetWindowsDirectoryW(winDir, MAX_PATH) &&
        SameDrive(winDir[0], t.letter[0])) {
        why = "目标是正在运行的系统盘（必须重启后脱机还原）";
        return false;
    }
    // 用户规格（2026-09-23）：**PE 里只要目标不是正在运行的系统盘，就应该就地还原**，
    // 不重启也不提示重启。PE 下目标分区通常并没被真正使用，但 FSCTL_LOCK_VOLUME 仍
    // 可能因各种句柄失败 → 会误判成"必须重启"（用户实测踩到）。所以 PE 下直接放行；
    // 正常 Windows 仍用卷锁判定，只有真的锁不上（分区被占用、无法就地还原）才提示重启。
    if (IsWinPE()) {
        why = "在 PE 中运行（目标非运行系统盘）→ 就地还原";
        // PE 下 FSCTL_LOCK_VOLUME 常因各种句柄失败 → 不能据此判"必须重启"（会把 PE
        // 误判成"要重启"，用户实测踩到）。所以**放行**，但仍**试一下锁**：锁不上只记
        // warn 继续（而不是拒绝）——这样格式化/wimlib 真失败时日志里有线索（问题清单 M-04）。
        std::wstring peDev = L"\\\\.\\" + t.letter + L":";
        HANDLE peH = CreateFileW(peDev.c_str(), GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
        if (peH == INVALID_HANDLE_VALUE) {
            LogWarn("PE: 打开目标卷失败 err=" + std::to_string(GetLastError()) +
                    "，仍尝试就地还原");
        } else {
            DWORD peRet = 0;
            if (DeviceIoControl(peH, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0,
                                &peRet, nullptr)) {
                DeviceIoControl(peH, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0,
                                &peRet, nullptr);
            } else {
                LogWarn("PE: 目标卷加锁失败（可能有句柄占用）err=" +
                        std::to_string(GetLastError()) +
                        "，仍尝试就地还原；若格式化/写入失败请先排除占用");
            }
            CloseHandle(peH);
        }
        return true;
    }
    std::wstring dev = L"\\\\.\\" + t.letter + L":";
    HANDLE h = CreateFileW(dev.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        why = "打开目标卷失败 err=" + std::to_string(GetLastError());
        return false;
    }
    DWORD ret = 0;
    BOOL locked = DeviceIoControl(h, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0,
                                  &ret, nullptr);
    if (locked)
        DeviceIoControl(h, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &ret,
                        nullptr);
    CloseHandle(h);
    why = locked ? "目标分区未被占用" : "目标分区正被使用（有打开的文件或句柄）";
    return locked != FALSE;
}
// 还原前空间预检（P1）—— 见 ops.h。**必须在格式化之前**调用。
int CheckRestoreSpace(const std::wstring& imagePath, int index,
                      const PartitionInfo& target, std::string& err) {
    WimEngine engine;
    if (!engine.ok())
        return 0;  // wimlib 起不来：不阻断（后面 apply 自己会报错）
    unsigned long long bytes = 0;
    int rc = engine.ImageSize(imagePath, index < 1 ? 1 : index, &bytes);
    if (rc != 0 || bytes == 0) {
        LogError("space precheck: cannot read image size (rc=" +
                 std::to_string(rc) + ") -> skip");
        return 0;  // 读不到大小 → 不阻断
    }
    // 内容量（已扣掉硬链接重复部分）+ 10% + 300MB（NTFS 元数据/目录索引/$LogFile 等开销）
    unsigned long long need = bytes + bytes / 10 + (300ull << 20);
    unsigned long long have = target.sizeBytes;
    char buf[320];
    snprintf(buf, sizeof(buf),
             "space precheck: content %.1f GB, need ~%.1f GB, target %.1f GB",
             bytes / 1073741824.0, need / 1073741824.0, have / 1073741824.0);
    LogInfo(buf);
    if (have && need > have) {
        snprintf(buf, sizeof(buf),
                 "目标分区空间不足：镜像解压后约需 %.1f GB（已含余量），"
                 "但目标分区只有 %.1f GB。\n请换更大的目标分区，或改用更小的镜像。",
                 need / 1073741824.0, have / 1073741824.0);
        err = buf;
        return 1;
    }
    return 0;
}

// 失败后的"下一步"（P8）—— 见 ops.h。按错误文本/退出码匹配，匹配不上返回空串。
std::string ErrorAdvice(int rc, const std::string& err) {
    auto has = [&err](const char* k) { return err.find(k) != std::string::npos; };
    // VSS 前置检查失败：消息里已自带完整处理步骤（services.msc / sc 两条路），
    // 必须放在最前短路 —— 否则下面的通用词分支（"管理员"，我们的提示词里也有）
    // 会追加"建议：以管理员身份运行"，而失败原因根本不是权限，纯属误导。
    if (has("卷影复制服务") || has("影子副本提供程序") || has("swprv"))
        return {};
    if (has("BitLocker") || has("bitlocker"))
        return "\n\n建议：目标盘启用了 BitLocker。请先在「管理员命令提示符」里挂起保护，"
               "然后重试：\n    manage-bde -protectors -disable X: -rebootcount 1\n"
               "（X 换成目标盘符。还原会覆盖该盘数据，之后不需要恢复保护。）";
    if (has("空间不足"))
        return "\n\n建议：换一个更大的目标分区，或改用内容更小的镜像。";
    if (has("写入未完成") || has("不完整"))
        return "\n\n建议：该镜像上次没有写完（备份中途中断过）。请重新做一次备份。";
    if (rc == 88 || has("concurrent") || has("正在被修改"))
        return "\n\n建议：备份源里有文件在持续变化（数据库/下载/云同步目录）。"
               "先停掉这些程序，或把它们所在目录加入排除清单后再备份。";
    if (has("镜像在目标分区内"))
        return "\n\n建议：把镜像文件移到别的分区 —— 它不能放在会被格式化的目标分区上。";
    if (has("管理员"))
        return "\n\n建议：以管理员身份运行（本程序要读写分区与引导）。";
    if (rc == 6)
        return "\n\n（任务已被用户取消，没有改动目标分区。）";
    return {};
}

// 历史记录（P7）：往 <exeDir>\logs\history.jsonl 追加一行（JSON Lines，外部/AI 好读）。
// 记"什么时候做了什么"：备份完成、就地还原、暂存还原（暂存的实际结果在救援层日志里）。
void AppendHistory(const char* action, const std::wstring& image,
                   const std::wstring& target, unsigned long long ms,
                   const char* detail) {
    std::wstring dir = ExeDir() + L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    HANDLE h = CreateFileW((dir + L"\\history.jsonl").c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    // 完整 JSON 字符串转义：`\` `"` 以及所有 < 0x20 的控制字符（\n\t\r\b\f 用短写，
    // 其余用 \u00xx）。原来只转义 `\` 和 `"` —— detail 一旦带换行就会写出非法 JSON
    //（问题清单 L-03）。
    auto esc = [](const std::string& s) {
        static const char* hx = "0123456789abcdef";
        std::string o;
        o.reserve(s.size() + 8);
        for (unsigned char c : s) {
            switch (c) {
                case '\\': o += "\\\\"; break;
                case '"': o += "\\\""; break;
                case '\n': o += "\\n"; break;
                case '\r': o += "\\r"; break;
                case '\t': o += "\\t"; break;
                case '\b': o += "\\b"; break;
                case '\f': o += "\\f"; break;
                default:
                    if (c < 0x20) {
                        o += "\\u00";
                        o += hx[(c >> 4) & 0xF];
                        o += hx[c & 0xF];
                    } else {
                        o += static_cast<char>(c);
                    }
            }
        }
        return o;
    };
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[1200];
    snprintf(buf, sizeof(buf),
             "{\"time\":\"%04u-%02u-%02u %02u:%02u:%02u\",\"action\":\"%s\","
             "\"image\":\"%s\",\"target\":\"%s\",\"elapsed_s\":%llu,"
             "\"version\":\"%s\",\"detail\":\"%s\"}\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             action, esc(W2U(image)).c_str(), esc(W2U(target)).c_str(),
             ms / 1000, SYSRECOVER_VERSION, detail ? detail : "");
    DWORD wrote = 0;
    WriteFile(h, buf, static_cast<DWORD>(strlen(buf)), &wrote, nullptr);
    CloseHandle(h);
}

int RunBackup(const BackupRequest& req, ProgressFn progress,
              std::string& err) {
    if (req.dest.empty()) {
        err = "缺少目标镜像路径";
        return 1;
    }
    if (!AcquireOpLock()) {
        err = "已有备份/还原实例在运行";
        return 1;
    }
    ProgressUpdate("backup", 0, "start");
    LogInfo("backup source=" + W2U(req.source) + " dest=" + W2U(req.dest) +
            " compress=" + req.compress);
    const ULONGLONG t0 = GetTickCount64();  // P7：历史记录用
    // 盘符根（C:/ 或 C:\）→ 热备：VSS 快照 + 排除配置（PIT-008/009）；
    // 也可用 req.snapshot 显式强制（例如备份正在使用的数据库目录）。
    bool snapshot = req.snapshot ||
                    (req.source.size() == 3 && req.source[1] == L':' &&
                     (req.source[2] == L'/' || req.source[2] == L'\\'));
    std::wstring errw;  // VSS 检查的多行提示（成功路径不会用到）
    // VSS 前置检查（用户 2026-09-26 规格）：服务停着就帮用户启动、起不来就给
    // 明确的处理指引（别等 wimlib 报 rc=89 让人干猜）；guard 析构把我们启动的
    // 服务停回原状（"完成后关闭"），覆盖下面所有成功/失败退出路径。
    vss::BackupGuard vss_guard;
    if (snapshot && !vss_guard.Ensure(errw)) {
        ReleaseOpLock();
        ProgressDone("backup", "failed");
        err = W2U(errw);  // 多行中文处理指引（GUI MessageBox / CLI 原样打印）
        LogError(err);
        return 1;
    }
    // 热备前把注册表 hive 刷盘：运行中的 hive 常处于"脏"状态
    // （REGF base block 的 primary_seq != secondary_seq），刷一次可让 VSS 抓到
    // 干净的 hive；即便仍是脏的，随包捕获的 *.LOG1/.LOG2 也能在开机时恢复
    // （PIT-056：这两者缺一，还原后就会黑屏起不来）。
    if (snapshot) {
        RegFlushKey(HKEY_LOCAL_MACHINE);
        RegFlushKey(HKEY_USERS);
        LogInfo("registry hives flushed before snapshot");
    }
    WimEngine engine;
    if (!engine.ok()) {
        ReleaseOpLock();
        ProgressDone("backup", "failed");
        err = "wimlib 初始化失败";
        return 1;
    }
    std::wstring cfg = EnsureExclusionConfig(req.source);
    if (cfg.empty())
        LogError("排除配置生成失败，热备可能因易失文件报 rc=88（PIT-009）");
    else
        LogInfo("exclusion config: " + W2U(cfg));
    int rc;
    if (req.append)
        rc = engine.Append(req.source, req.dest, req.compress, req.name,
                           snapshot, cfg, progress);
    else
        rc = engine.Capture(req.source, req.dest, req.compress, req.name,
                            snapshot, cfg, progress);
    if (rc != 0) {
        ReleaseOpLock();
        ProgressDone("backup", "failed");
        char buf[128];
        snprintf(buf, sizeof(buf), "备份失败(rc=%d): ", rc);
        err = buf;
        err += W2U(WimEngine::ErrorString(rc));
        LogError(err);
        return 1;
    }
    if (req.verify) {
        ProgressUpdate("backup", 99, "verify");
        int vrc = engine.Verify(req.dest);
        if (vrc != 0) {
            ReleaseOpLock();
            ProgressDone("backup", "verify-failed");
            char buf[128];
            snprintf(buf, sizeof(buf), "备份已写出但校验失败(rc=%d): ", vrc);
            err = buf;
            err += W2U(WimEngine::ErrorString(vrc));
            LogError(err);
            return 5;
        }
        LogInfo("backup verify passed");
    }
    // 即便没要求 --verify，也检查一下镜像是否"写入完成"（防止中断留下半个文件，
    // 被当成有效镜像去还原 → rc=84 黑屏，PIT-057）
    {
        WimEngine probe;
        std::wstring why;
        if (probe.Probe(req.dest, why) != 0) {
            ReleaseOpLock();
            ProgressDone("backup", "failed");
            err = "备份镜像不可用（写入未完成）：";
            err += W2U(why);
            LogError(err);
            return 1;
        }
    }
    ReleaseOpLock();
    ProgressDone("backup", "done");
    LogInfo("backup done");
    AppendHistory("backup", req.dest, req.source, GetTickCount64() - t0, "ok");
    return 0;
}

int StageRestore(const RestoreRequest& req, std::string& err,
                 bool* needReboot, ProgressFn progress) {
    if (req.image.empty()) {
        err = "缺少镜像路径";
        return 1;
    }
    // 0) 镜像可用性检查：拒绝"上次没写完/不完整"的镜像（否则 Linux 侧 apply
    //    rc=84 WIM_IS_INCOMPLETE，而目标分区已被格式化 → 开机黑屏，PIT-057）
    {
        WimEngine probe;
        std::wstring why;
        if (probe.Probe(req.image, why) != 0) {
            err = "镜像不可用，请重新备份：";
            err += W2U(why);
            LogError(err);
            return 5;
        }
    }
    // 1) 定位目标分区与所在磁盘
    DiskInfo tdisk;
    PartitionInfo target;
    bool found = false;
    for (const auto& d : EnumerateDisks()) {
        if ((int)d.index != req.disk)
            continue;
        for (const auto& p : d.parts) {
            if ((int)p.partNumber != req.part)
                continue;
            tdisk = d;
            target = p;
            found = true;
        }
    }
    if (!found) {
        char buf[96];
        snprintf(buf, sizeof(buf), "找不到目标分区 磁盘%d 分区%d", req.disk,
                 req.part);
        err = buf;
        return 1;
    }
    // 1b) 软件/镜像若在目标分区（会被格式化）上 → 镜像移动、软件复制到数据盘。
    //     放在 <数据盘>\SysRecover\（持久保留，不参与还原后清理），绝不丢用户镜像。
    std::wstring imagePath = req.image;
    {
        std::wstring dataDrive = FindDataDrive();
        wchar_t tgtLetter = target.letter.empty() ? 0 : target.letter[0];
        std::wstring keep = dataDrive.empty() ? L"" : dataDrive + L"SysRecover";
        // 8) 镜像在目标分区 → 必须移走，否则还原时被一起格式化掉
        if (tgtLetter && imagePath.size() > 1 && imagePath[1] == L':' &&
            SameDrive(imagePath[0], tgtLetter)) {
            if (keep.empty()) {
                err = "镜像在目标分区内，且找不到可存放它的数据盘；请先手动移走镜像";
                return 4;
            }
            CreateDirectoryW(keep.c_str(), nullptr);
            size_t sl = imagePath.find_last_of(L"\\/");
            std::wstring fn =
                sl == std::wstring::npos ? imagePath : imagePath.substr(sl + 1);
            std::wstring dst = keep + L"\\" + fn;
            LogInfo("image on target partition; moving to " + W2U(dst));
            if (!MoveFileExW(imagePath.c_str(), dst.c_str(),
                             MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING)) {
                err = "镜像在目标分区内，搬到数据盘失败（空间不足？）；请先手动移走镜像";
                return 4;
            }
            imagePath = dst;
        }
        // 7) 软件在目标分区 → 复制一份到数据盘（还原后 C: 被覆盖，留个副本）
        std::wstring exeDir0 = ExeDir();
        if (tgtLetter && exeDir0.size() > 1 && exeDir0[1] == L':' &&
            SameDrive(exeDir0[0], tgtLetter) && !keep.empty()) {
            std::wstring appDst = keep + L"\\app";
            LogInfo("software on target partition; copying to " + W2U(appDst));
            if (!CopyDir(exeDir0, appDst))
                LogError("复制软件到数据盘失败（不致命，仅副本未成功）");
        }
    }
    // 2) 安全门禁（fail-closed，§11）
    std::string reason = CheckRestoreTarget(target, imagePath);
    if (!reason.empty()) {
        err = reason;
        return 4;
    }
    // 2.05) 目标盘健康（PIT-086，用户 2026-09-26）：坏盘 → 记日志（**展示**由 GUI 弹框 /
    //       CLI 打印负责；这里不阻断 —— 与 BitLocker 一致，取不到 SMART 也放行）。
    {
        std::string hw = CheckDiskHealth((int)target.diskIndex);
        if (!hw.empty())
            LogWarn("target disk health: " + hw);
    }
    // 2.1) 空间预检（P1）：镜像未压缩大小 vs 目标分区大小。
    //      必须在这里（**格式化之前**）拦下 —— 否则会出现"数据没了、系统也没装上"。
    //      就地还原与"暂存+重启"两条路都从这一点过，所以放在分支之前。
    {
        std::string spErr;
        if (CheckRestoreSpace(imagePath, req.index, target, spErr) == 1) {
            err = spErr;
            LogError("space precheck failed: " + spErr);
            return 1;
        }
    }
    if (!AcquireOpLock()) {
        err = "已有备份/还原实例在运行";
        return 1;
    }
    ProgressUpdate("restore", 0, "start");
    LogInfo("restore stage image=" + W2U(imagePath) + " disk=" +
            std::to_string(req.disk) + " part=" + std::to_string(req.part));

    // 2.5) 目标分区没被占用（在 PE 里、或还原到非系统盘）→ **就地还原**，不重启；
    //      否则维持原设计：暂存任务 + 重启进 Linux 救援层执行（PIT-064）。
    {
        std::string why;
        if (CanRestoreInPlace(target, why)) {
            LogInfo(std::string("restore mode: in-place (") + why + ")");
            const ULONGLONG t1 = GetTickCount64();
            int rc = RunDirectRestore(req, target, imagePath, err, progress);
            if (needReboot)
                *needReboot = false;
            ReleaseOpLock();
            AppendHistory("restore-inplace", req.image, imagePath,
                          GetTickCount64() - t1, rc == 0 ? "ok" : "failed");
            return rc;
        }
        LogInfo(std::string("restore mode: staged reboot (") + why + ")");
        // 网络镜像防呆（问题清单 D1/H1）：暂存+重启后由 **Linux 救援层**读镜像，而救援层
        // 访问不到网络（UNC / 映射网络驱动器）→ 必然 image not found。这里**提前拒绝**
        //（就地还原不在此列 —— 那是 Windows 自己读，网络没问题）。
        if (IsNetworkPath(imagePath)) {
            err =
                "镜像在网络路径上（UNC 或映射网络驱动器）。还原系统盘需要重启进救援层，"
                "而救援层无法访问网络 —— 请先把镜像复制到**本地分区**（如 D:\\）再还原。";
            LogError("staged restore rejected: image on network path");
            AppendHistory("restore-rejected", req.image, imagePath, 0,
                          "image on network path");
            ReleaseOpLock();
            ProgressDone("restore", "failed");
            return 2;
        }
        AppendHistory("restore-staged", req.image, imagePath, 0,
                      "staged; 实际结果见救援层日志");
    }

    std::wstring exeDir = ExeDir();
    std::wstring logsDir = exeDir + L"\\logs";
    CreateDirectoryW(logsDir.c_str(), nullptr);
    if (req.repairBoot)
        BcdExport(logsDir + L"\\bcd-backup");  // 改 BCD 前先备份（§11）

    // 3) 组装任务（字段对齐旧 C# StageRestoreAsync）
    RestoreTask t;
    t.imagePath = imagePath;
    t.imageIndex = req.index < 1 ? 1 : req.index;
    t.targetGuid = target.guid;
    t.targetOffset = target.offsetBytes;
    t.targetSize = target.sizeBytes;
    t.targetDiskSerial = tdisk.serial;
    t.targetDisk = req.disk;
    t.targetPart = req.part;
    t.ptType = tdisk.style == PartitionStyle::GPT ? "gpt" : "mbr";
    t.repairBoot = req.repairBoot;
    t.targetDiskName = tdisk.model;
    t.targetDiskSize = tdisk.sizeBytes;
    t.targetFs = target.fs;
    t.targetVolLabel = target.label;
    t.softwarePath = ExePath();
    // 诊断日志目录（PIT-059）：优先软件目录本身；软件在目标盘（会被格式化）
    // 或只读介质上时，回退到数据盘 <数据盘>\ZJRESTORE（建好，保留给排错用）。
    {
        wchar_t exLetter = exeDir.size() > 1 ? exeDir[0] : 0;
        wchar_t tgtLetter = target.letter.empty() ? 0 : target.letter[0];
        wchar_t exRoot[4] = {exLetter, L':', L'\\', 0};
        bool onTarget =
            exLetter && tgtLetter && SameDrive(exLetter, tgtLetter);
        bool fixedDisk = exLetter && GetDriveTypeW(exRoot) == DRIVE_FIXED;
        if (!onTarget && fixedDisk) {
            t.softwareDir = exeDir;
        } else {
            std::wstring dataDrive = FindDataDrive();
            if (dataDrive.empty())
                t.softwareDir = exeDir;
            else
                t.softwareDir = dataDrive + kRecoveryDir;  // D:\ZJRESTORE
            CreateDirectoryW(t.softwareDir.c_str(), nullptr);
            LogInfo("software dir unusable for logs; log dir=" +
                    W2U(t.softwareDir));
        }
    }
    // 镜像位置：盘符 → 所在分区 GUID + 相对路径（正斜杠）
    if (imagePath.size() > 3 && imagePath[1] == L':') {
        wchar_t letter = towupper(imagePath[0]);
        for (const auto& d : EnumerateDisks())
            for (const auto& p : d.parts)
                if (!p.letter.empty() && towupper(p.letter[0]) == letter)
                    t.imagePartGuid = p.guid;
        std::wstring rel = imagePath.substr(3);
        for (auto& c : rel)
            if (c == L'\\')
                c = L'/';
        t.imageRelPath = rel;
    }

    // 4) _zjresy 日志写目标分区根（restore.sh 主契约；缺日志 Linux 侧找不到
    //    目标，视为致命——较旧 C# 的 warn 更严格，fail-closed）
    if (target.letter.empty()) {
        ReleaseOpLock();
        ProgressDone("restore", "failed");
        err = "目标分区无盘符，无法写恢复日志（Linux 侧将无法定位目标）";
        LogError(err);
        return 1;
    }
    std::string tlog;
    if (!WriteRestoreLog(target.letter[0], t, tlog)) {
        ReleaseOpLock();
        ProgressDone("restore", "failed");
        err = "写恢复日志失败（需管理员权限写 " + W2U(target.letter) + ":\\）";
        LogError(err + " / " + tlog);
        return 1;
    }
    // 5) 任务文件（**副契约**）：优先写 exe 目录；只读介质（光盘）或写不了时回退到
    //    <数据盘>\ZJRESTORE\（救援层会扫描各分区找它，PIT-035）。
    //    ⚠️ **写不了也不致命**：主契约是**目标分区根**上的 _zjresy 日志，救援层能从
    //    日志字段读出全部参数（PIT-035 的回退）。2026-09-23 实测：用户从**光盘**运行，
    //    这一步失败 + 被当成致命错误 → 还原根本没执行 ✗（而日志其实已经写好了 ✓）。
    {
        std::string wtlog;
        bool okTask = WriteRestoreTask(exeDir, t, wtlog);
        std::wstring used = exeDir;
        if (!okTask) {
            std::wstring dataDrive = FindDataDrive();
            std::wstring alt =
                dataDrive.empty() ? std::wstring() : dataDrive + L"ZJRESTORE";
            if (!alt.empty()) {
                CreateDirectoryW(alt.c_str(), nullptr);
                wtlog.clear();
                if (WriteRestoreTask(alt, t, wtlog)) {
                    okTask = true;
                    used = alt;
                }
            }
        }
        if (!okTask)
            LogError("restore task write failed (non-fatal; the target-root "
                     "log is the primary contract): " + wtlog);
        else
            LogInfo("restore task written to " + W2U(used));
    }
    // 6) 引导层 + 单次启动（仅正常 Windows，§2 禁令3）
    if (req.repairBoot) {
        std::string blog;
        if (IsUefiFirmware()) {
            // UEFI/GPT：写**固件启动项**（Boot#### + BootOrder），由固件直接加载
            // 内核（EFI stub）并把 OptionalData 当内核命令行 —— 全程不经 bootmgr，
            // 因此没有 bootapp 的 0xc000007b（PIT-060）。ESP 不受还原影响。
            SbMode sbm = CurrentSbMode();
            if (sbm == SbMode::Shim && !IsMokEnrolled(exeDir)) {
                // Secure Boot 开着且走 shim 链，但用户还没做过那次一次性密钥注册：
                // 此时重启会被 MokManager 拦住，还原根本不会跑（看起来"什么都没
                // 发生"）。必须在**动目标分区之前**先拦住并说清怎么注册。
                // （走 bootapp 链不需要注册，见 PIT-063。）
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                err = "本机开启了 Secure Boot，但还没注册启动密钥：请先点"
                      "「安装启动还原」，重启一次，在蓝底界面选 "
                      "Enroll key -> 选 zj-mok.cer -> 注册，之后回来再还原";
                LogError(err);
                return 1;
            }
            PartitionInfo esp;
            if (!FindEspPartition(esp)) {
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                err = "UEFI 机器未找到 ESP 分区，无法部署引导层";
                LogError(err);
                return 1;
            }
            std::wstring espRoot = MountEsp(blog);
            if (espRoot.empty()) {
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                err = "无法给 ESP 分配盘符（mountvol X: /s 失败）";
                LogError(err + " / " + blog);
                return 1;
            }
            bool ok = InstallUefiBootEntry(espRoot, exeDir, blog);
            // 修 UEFI 引导：用微软官方 bcdboot 重新生成 ESP 上的 bootmgfw.efi + BCD
            // （指向系统盘）。等价于"万能镜像还原后修 ESP 引导"，让还原后的系统能进。
            {
                // 用**目标分区**的 Windows 目录（暂存时它还是旧系统，但引导文件一样）
                std::wstring winDir =
                    std::wstring(1, target.letter[0]) + L":\\Windows";
                std::string out;
                std::wstring args = winDir + L" /s " +
                                    std::wstring(1, espRoot[0]) + L": /f UEFI";
                int brc = RunProcess(SysToolPath(L"bcdboot.exe"), args, out);
                LogInfo("bcdboot (UEFI) rc=" + std::to_string(brc) + " / " + out);
            }
            UnmountEsp(espRoot, blog);
            LogInfo(std::string("uefi boot entry: ") + (ok ? "ok" : "FAIL") +
                    " / " + blog);
            if (!ok) {
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                err = "安装 UEFI 引导层失败: " + blog;
                LogError(err);
                return 1;
            }
        } else {
            // BIOS/MBR：部署到**目标分区**（通常 C:）；救援文件会被本次还原格式化掉，
            // 所以不必去动数据盘（D:/E:）的根目录。
            std::wstring deployDrive = std::wstring(1, target.letter[0]) + L":\\";
            // 每次都重装（幂等）：确保 menu.lst/bootfiles/initramfs 随本版本刷新。
            if (!InstallBootLayer(deployDrive, exeDir, blog)) {
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                err = "安装引导层失败: " + blog;
                LogError(err);
                return 1;
            }
            // 引导补全：生成 <目标>\ZJRESTORE\bootfix\（Linux 侧在 mkntfs 前拷进内存）
            {
                std::wstring rec = deployDrive + kRecoveryDir;
                std::string flog;
                bool bok = PrepareBootFixFiles(rec, flog);
                LogInfo(std::string("bootfix: ") + (bok ? "ok" : "FAILED") +
                        " / " + flog);
                if (!bok)
                    LogError("prepare bootfix failed: " + flog);
            }
        }
        // 单次启动（都只生效一次）：
        //   UEFI + shim/direct → 固件 BootNext
        //   UEFI + bootapp     → BCD bootsequence（install 里已设好，不重复设）
        //   BIOS               → BCD bootsequence
        if (IsUefiFirmware() && CurrentSbMode() != SbMode::Bootapp) {
            if (!SetUefiBootNext(blog)) {
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                err = "设置固件单次启动失败: " + blog;
                LogError(err);
                return 1;
            }
        } else if (!IsUefiFirmware() && !BcdSetBootsequence(RecoveryGuid())) {
            ReleaseOpLock();
            ProgressDone("restore", "failed");
            err = "设置单次启动失败";
            LogError(err);
            return 1;
        }
    }
    ReleaseOpLock();
    ProgressDone("restore", "staged");
    LogInfo("restore staged, reboot to execute");
    return 0;
}

}  // namespace sysrecover
