// 备份/还原共享操作层实现。语义对齐 CLI（Phase 2-4 已验证）与旧 C# 编排器。
#include "../common/i18n.h"
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
#include "../common/pathutil.h"
#include "../common/cpucap.h"
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

// 取一个**可用的 ESP 根**（形如 "X:\\"）。顺序（docs/15 · P4）：
//   ① `mountvol <空闲盘符>: /s` —— 固件**真正启动**的那个分区。关键点：它按固件
//      启动项找，**不依赖分区类型 GUID**，所以 DiskGenius 把 ESP 标成 Basic Data
//      时它照样挂得上；
//   ② 类型 GUID 匹配到的分区若**已有盘符**，直接用（mountvol 失败时的兜底）；
//   ③ 回退：FAT 分区且根下有 \EFI\Microsoft\Boot\bootmgfw.efi（故障 B 的正解）。
// guidMatched=true 表示"GPT 类型 GUID 也认对了"（正常机器；false 时调用方只记警告）。
bool AcquireEspRoot(std::wstring& espRoot, bool& guidMatched, std::string& log) {
    PartitionInfo esp;
    guidMatched = FindEspPartition(esp);
    espRoot = MountEsp(log);
    if (!espRoot.empty())
        return true;
    if (guidMatched && !esp.letter.empty()) {
        std::wstring root = esp.letter + L":\\";
        if (GetFileAttributesW(root.c_str()) != INVALID_FILE_ATTRIBUTES) {
            espRoot = root;
            log += "ESP via existing drive letter\n";
            return true;
        }
    }
    PartitionInfo fb;
    if (FindEspPartitionFallback(fb) && !fb.letter.empty()) {
        espRoot = fb.letter + L":\\";
        log += "ESP via fallback (FAT partition with \\EFI boot files)\n";
        return true;
    }
    return false;
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
            err = Tr("就地还原：格式化目标分区失败（") + out + Tr("）");
            LogError(err);
            ProgressDone("restore", "failed");
            return 1;
        }
    }
    // 2) 应用镜像（目录模式）
    WimEngine wim;
    if (!wim.ok()) {
        err = Tr("wimlib 初始化失败");
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
        err = Tr("就地还原：应用镜像失败 rc=") + std::to_string(arc) + " (" +
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
                // 找 ESP（含回退）+ 先预检目标模板（docs/15 · P2/P4）
                std::wstring espRoot;
                bool guidMatched = false;
                std::string blog;
                std::string detail;
                if (!AcquireEspRoot(espRoot, guidMatched, blog)) {
                    err = Tr("就地还原已完成，但找不到可用的 ESP 引导分区，无法修复引导：\n") +
                          W2U(DescribePartitions());
                    LogError(err);
                    ProgressDone("restore", "failed");
                    return 1;
                }
                if (!guidMatched)
                    LogWarn("ESP partition type is not EFI System; used fallback");
                if (!TargetBcdTemplateOk(letter[0], detail)) {
                    err = Tr("就地还原已完成，但目标系统缺少引导模板（") + detail +
                          Tr("），无法生成引导。请换用完整系统镜像。");
                    UnmountEsp(espRoot, blog);
                    LogError(err);
                    ProgressDone("restore", "failed");
                    return 1;
                }
                int brc = RunProcess(
                    SysToolPath(L"bcdboot.exe"),
                    winDir + L" /s " + std::wstring(1, espRoot[0]) + L": /f UEFI",
                    out);
                LogInfo("bcdboot (UEFI) rc=" + std::to_string(brc) + " / " + out);
                if (brc != 0) {
                    // 目标分区**已经格式化并写好系统**了 → 只能明确报错 + 指路
                    //（`repair-boot` 可在 PE/正常系统里再修，不必重装）
                    err = Tr("就地还原已完成，但写入 ESP 引导失败（bcdboot rc=") +
                          std::to_string(brc) + Tr("）：") + out +
                          Tr("\n可进入 PE 用 `SysRecover.exe repair-boot` 重试修复引导。");
                    UnmountEsp(espRoot, blog);
                    LogError(err);
                    ProgressDone("restore", "failed");
                    return 1;
                }
                if (!VerifyEspBcd(espRoot, detail)) {
                    err = Tr("就地还原已完成，但 ESP 引导校验未通过：") + detail +
                          Tr("\n可进入 PE 用 `SysRecover.exe repair-boot` 重试修复引导。");
                    UnmountEsp(espRoot, blog);
                    LogError(err);
                    ProgressDone("restore", "failed");
                    return 1;
                }
                UnmountEsp(espRoot, blog);
            } else {
                int brc = RunProcess(SysToolPath(L"bcdboot.exe"),
                                     winDir + L" /s " + letter + L": /f BIOS",
                                     out);
                LogInfo("bcdboot (BIOS) rc=" + std::to_string(brc) + " / " +
                        out);
                if (brc != 0) {
                    err = Tr("就地还原已完成，但写入引导失败（bcdboot rc=") +
                          std::to_string(brc) + Tr("）：") + out;
                    LogError(err);
                    ProgressDone("restore", "failed");
                    return 1;
                }
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
bool CanRestoreInPlace(const PartitionInfo& t, std::string& why,
                       InPlaceReason* reason) {
    // reason：给调用方一个**稳定码**来选显示文案（GUI 确认框原来靠
    // `why.find("系统盘")` 匹配中文子串，消息一翻译就失效 —— PLAN §14 M1）
    auto setr = [&](InPlaceReason r) { if (reason) *reason = r; };
    if (t.letter.empty()) {
        setr(INPLACE_NO_LETTER);
        why = Tr("目标分区没有盘符（无法就地写）");
        return false;
    }
    wchar_t winDir[MAX_PATH] = {};
    if (GetWindowsDirectoryW(winDir, MAX_PATH) &&
        SameDrive(winDir[0], t.letter[0])) {
        setr(INPLACE_SYSTEM_DISK);
        why = Tr("目标是正在运行的系统盘（必须重启后脱机还原）");
        return false;
    }
    // 用户规格（2026-09-23）：**PE 里只要目标不是正在运行的系统盘，就应该就地还原**，
    // 不重启也不提示重启。PE 下目标分区通常并没被真正使用，但 FSCTL_LOCK_VOLUME 仍
    // 可能因各种句柄失败 → 会误判成"必须重启"（用户实测踩到）。所以 PE 下直接放行；
    // 正常 Windows 仍用卷锁判定，只有真的锁不上（分区被占用、无法就地还原）才提示重启。
    if (IsWinPE()) {
        why = Tr("在 PE 中运行（目标非运行系统盘）→ 就地还原");
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
        setr(INPLACE_PE);
        return true;
    }
    std::wstring dev = L"\\\\.\\" + t.letter + L":";
    HANDLE h = CreateFileW(dev.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        setr(INPLACE_OPEN_FAIL);
        why = Tr("打开目标卷失败 err=") + std::to_string(GetLastError());
        return false;
    }
    DWORD ret = 0;
    BOOL locked = DeviceIoControl(h, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0,
                                  &ret, nullptr);
    if (locked)
        DeviceIoControl(h, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &ret,
                        nullptr);
    CloseHandle(h);
    setr(locked ? INPLACE_OK : INPLACE_LOCKED);
    why = locked ? Tr("目标分区未被占用") : Tr("目标分区正被使用（有打开的文件或句柄）");
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
                 Tr("目标分区空间不足：镜像解压后约需 %.1f GB（已含余量），" "但目标分区只有 %.1f GB。\n请换更大的目标分区，或改用更小的镜像。"),
                 need / 1073741824.0, have / 1073741824.0);
        err = buf;
        return 1;
    }
    return 0;
}

// 失败后的"下一步"（P8）已迁到 advice.cpp —— 见 advice.h。原因（PLAN §14 M1）：
// 本文件里旧版靠「中文关键词匹配中文消息」，消息一被翻译就全部失配 → 静默不给建议。
// 下面这行 = 把老签名**声明为已删除**：谁在本翻译单元里再按旧签名调用，编译期就报错。
std::string ErrorAdvice(int rc, const std::string& err) = delete;

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
              std::string& err, ErrAdvice* adv) {
    if (adv) *adv = ADV_NONE;
    if (req.dest.empty()) {
        err = Tr("缺少目标镜像路径");
        return 1;
    }
    // 源路径归一（2026-09-28 用户 CLI 复现：`--source C:` 备份报 rc=47）：
    //   · Win32 里 `C:` 是“该盘的**当前目录**”，不是根，也不带 wimlib 要求的
    //     尾斜杠 → 盘符根判定落空 → 没走 VSS 热备，直接扫活动系统；
    //   · 扫活动系统会在某个正被占用的文件上失败，wimlib 只回
    //     `rc=47 Failed to open a file`，看不出是谁的锅。
    // 统一成 `C:/` 后，`C:` / `C:\` / `C:/` 三条写法等价（GUI 本来就传 `X:/`）。
    const std::wstring source = NormalizeVolumeRoot(req.source);
    const bool volumeRoot = IsVolumeRoot(source);
    // 目标目录不存在也要建出来：`D:\backup\x.esd` 里 D:\backup 没建时，
    // wimlib 建不出输出文件，同样只报 rc=47（2026-09-28 实测 3 秒即失败）。
    {
        std::wstring dir = ParentDir(req.dest);
        if (!dir.empty() && !MakeDirTree(dir)) {
            err = std::string(Tr("无法创建备份目录：")) + W2U(dir);
            LogError(err);
            return 1;
        }
    }
    if (!AcquireOpLock()) {
        err = Tr("已有备份/还原实例在运行");
        return 1;
    }
    ProgressUpdate("backup", 0, "start");
    LogInfo("backup source=" + W2U(source) + " dest=" + W2U(req.dest) +
            " compress=" + req.compress +
            (volumeRoot ? " (volume root -> VSS hot backup)" : ""));
    const ULONGLONG t0 = GetTickCount64();  // P7：历史记录用
    // 盘符根（C:/ 或 C:\）→ 热备：VSS 快照 + 排除配置（PIT-008/009）；
    // 也可用 req.snapshot 显式强制（例如备份正在使用的数据库目录）。
    bool snapshot = req.snapshot || volumeRoot;
    // CPU 硬上限（BackupRequest::cpuCap，GUI 的「限制CPU」下拉）：在动数据之前
    // 设好，所有成功/失败退出路径都由 guard 复位回“不限”。
    struct CpuCapGuard {
        int pct;
        explicit CpuCapGuard(int p) : pct(p) {
            if (pct <= 0) return;
            std::string why;
            if (!SetCpuCap(pct, &why))
                LogWarn("cpu cap not applied: " + why);
            else
                LogInfo("cpu cap = " + std::to_string(pct) + "%");
        }
        ~CpuCapGuard() {
            if (pct <= 0) return;
            std::string why;
            SetCpuCap(0, &why);
        }
    } cpuGuard(req.cpuCap);
    std::wstring errw;  // VSS 检查的多行提示（成功路径不会用到）
    // VSS 前置检查（用户 2026-09-26 规格）：服务停着就帮用户启动、起不来就给
    // 明确的处理指引（别等 wimlib 报 rc=89 让人干猜）；guard 析构把我们启动的
    // 服务停回原状（"完成后关闭"），覆盖下面所有成功/失败退出路径。
    vss::BackupGuard vss_guard;
    if (snapshot && !vss_guard.Ensure(errw)) {
        ReleaseOpLock();
        ProgressDone("backup", "failed");
        if (adv) *adv = ADV_VSS;  // 消息自带处理步骤 → ErrorAdvice 不再追加
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
        err = Tr("wimlib 初始化失败");
        return 1;
    }
    std::wstring cfg = EnsureExclusionConfig(source);
    if (cfg.empty())
        LogError("排除配置生成失败，热备可能因易失文件报 rc=88（PIT-009）");
    else
        LogInfo("exclusion config: " + W2U(cfg));
    int rc;
    if (req.append)
        rc = engine.Append(source, req.dest, req.compress, req.name,
                           snapshot, cfg, progress);
    else
        rc = engine.Capture(source, req.dest, req.compress, req.name,
                            snapshot, cfg, progress);
    if (rc != 0) {
        ReleaseOpLock();
        ProgressDone("backup", "failed");
        char buf[128];
        snprintf(buf, sizeof(buf), Tr("备份失败(rc=%d): "), rc);
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
            snprintf(buf, sizeof(buf), Tr("备份已写出但校验失败(rc=%d): "), vrc);
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
            if (adv) *adv = ADV_INCOMPLETE;
            err = Tr("备份镜像不可用（写入未完成）：");
            err += W2U(why);
            LogError(err);
            return 1;
        }
    }
    ReleaseOpLock();
    ProgressDone("backup", "done");
    LogInfo("backup done");
    AppendHistory("backup", req.dest, source, GetTickCount64() - t0, "ok");
    return 0;
}

int StageRestore(const RestoreRequest& req, std::string& err,
                 bool* needReboot, ProgressFn progress, ErrAdvice* adv) {
    if (adv) *adv = ADV_NONE;
    if (req.image.empty()) {
        err = Tr("缺少镜像路径");
        return 1;
    }
    // 0) 镜像可用性检查：拒绝"上次没写完/不完整"的镜像（否则 Linux 侧 apply
    //    rc=84 WIM_IS_INCOMPLETE，而目标分区已被格式化 → 开机黑屏，PIT-057）
    {
        WimEngine probe;
        std::wstring why;
        if (probe.Probe(req.image, why) != 0) {
            if (adv) *adv = ADV_INCOMPLETE;
            err = Tr("镜像不可用，请重新备份：");
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
        snprintf(buf, sizeof(buf), Tr("找不到目标分区 磁盘%d 分区%d"), req.disk,
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
                if (adv) *adv = ADV_IMAGE_IN_TARGET;
                err = Tr("镜像在目标分区内，且找不到可存放它的数据盘；请先手动移走镜像");
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
                err = Tr("镜像在目标分区内，搬到数据盘失败（空间不足？）；请先手动移走镜像");
                if (adv) *adv = ADV_SPACE;  // 旧关键词版命中的也是"空间不足"（保留原建议）
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
    std::string reason = CheckRestoreTarget(target, imagePath, adv);
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
            if (adv) *adv = ADV_SPACE;
            LogError("space precheck failed: " + spErr);
            return 1;
        }
    }
    if (!AcquireOpLock()) {
        err = Tr("已有备份/还原实例在运行");
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
                Tr("镜像在网络路径上（UNC 或映射网络驱动器）。还原系统盘需要重启进救援层，" "而救援层无法访问网络 —— 请先把镜像复制到**本地分区**（如 D:\\）再还原。");
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
        err = Tr("目标分区无盘符，无法写恢复日志（Linux 侧将无法定位目标）");
        LogError(err);
        return 1;
    }
    std::string tlog;
    if (!WriteRestoreLog(target.letter[0], t, tlog)) {
        ReleaseOpLock();
        ProgressDone("restore", "failed");
        if (adv) *adv = ADV_ADMIN;
        err = Tr("写恢复日志失败（需管理员权限写 ") + W2U(target.letter) + Tr(":\\）");
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
                err = Tr("本机开启了 Secure Boot，但还没注册启动密钥：请先点" "「安装菜单」，重启一次，在蓝底界面选 " "Enroll key -> 选 zj-mok.cer -> 注册，之后回来再还原");
                LogError(err);
                return 1;
            }
            std::wstring espRoot;
            bool guidMatched = false;
            std::string detail;
            if (!AcquireEspRoot(espRoot, guidMatched, blog)) {
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                err = Tr("UEFI 机器未找到可用的 ESP 引导分区，无法部署引导层。\n"
                         "常见原因：DiskGenius 重建分区时漏建 ESP，或把 ESP 标成了"
                         " Basic Data（应改为 EFI System 类型）。\n当前分区表：\n") +
                      W2U(DescribePartitions());
                LogError(err);
                return 1;
            }
            if (!guidMatched)
                LogWarn("ESP partition type is not EFI System; used fallback "
                        "(mountvol/\\EFI files)");
            // ① 目标系统盘得有 bcdboot 要的模板（docs/15 · P2c）
            if (!TargetBcdTemplateOk(target.letter[0], detail)) {
                UnmountEsp(espRoot, blog);
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                err = Tr("目标系统缺少引导模板（") + detail +
                      Tr("）：bcdboot 无法生成引导。多见于第三方精简/万能镜像，"
                         "请换用完整系统镜像。");
                LogError(err);
                return 1;
            }
            // ② ESP 还得放得下 bcdboot 要写的 ~9MB（docs/15 · P2a）
            if (!EspHasRoomForBcdboot(espRoot, detail)) {
                UnmountEsp(espRoot, blog);
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                err = Tr("ESP 空间不足，无法修复引导（") + detail +
                      Tr("）：请先在 Windows 里点「删除启动还原」清掉 "
                         "\\EFI\\ZJRESTORE\\ 旧版残留，或用 diskpart 扩大 ESP。");
                LogError(err);
                return 1;
            }
            // ③ **先跑 bcdboot**（此刻 ESP 空间最富裕），再放我们 33MB 的救援载荷
            //    —— 顺序反了就会被自己的载荷挤掉空间（docs/15 · P2b）。
            bool ok = true;
            {
                // 用**目标分区**的 Windows 目录（暂存时它还是旧系统，但引导文件一样）
                std::wstring winDir =
                    std::wstring(1, target.letter[0]) + L":\\Windows";
                std::string out;
                std::wstring args = winDir + L" /s " +
                                    std::wstring(1, espRoot[0]) + L": /f UEFI";
                int brc = RunProcess(SysToolPath(L"bcdboot.exe"), args, out);
                LogInfo("bcdboot (UEFI) rc=" + std::to_string(brc) + " / " + out);
                if (brc != 0) {
                    // 返回码曾经被丢弃（静默失败）→ 现在 fail-closed（docs/15 · P1）
                    UnmountEsp(espRoot, blog);
                    ReleaseOpLock();
                    ProgressDone("restore", "failed");
                    err = Tr("写入 ESP 引导失败（bcdboot rc=") +
                          std::to_string(brc) + Tr("）：") + out;
                    LogError(err);
                    return 1;
                }
                // ④ 产物断言：displayorder 非空 + 有 winload 条目 + 关键文件齐全
                if (!VerifyEspBcd(espRoot, detail)) {
                    UnmountEsp(espRoot, blog);
                    ReleaseOpLock();
                    ProgressDone("restore", "failed");
                    err = Tr("ESP 引导校验未通过（未真正修好，已中止，未动目标分区）：") +
                          detail;
                    LogError(err);
                    return 1;
                }
            }
            // ⑤ 救援载荷（内核 + initramfs ≈ 33MB；它自带空间检查）
            ok = InstallUefiBootEntry(espRoot, exeDir, blog);
            UnmountEsp(espRoot, blog);
            LogInfo(std::string("uefi boot entry: ") + (ok ? "ok" : "FAIL") +
                    " / " + blog);
            if (!ok) {
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                err = Tr("安装 UEFI 引导层失败: ") + blog;
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
                err = Tr("安装引导层失败: ") + blog;
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
                err = Tr("设置固件单次启动失败: ") + blog;
                LogError(err);
                return 1;
            }
        } else if (!IsUefiFirmware() && !BcdSetBootsequence(RecoveryGuid())) {
            ReleaseOpLock();
            ProgressDone("restore", "failed");
            err = Tr("设置单次启动失败");
            LogError(err);
            return 1;
        }
    }
    ReleaseOpLock();
    ProgressDone("restore", "staged");
    LogInfo("restore staged, reboot to execute");
    return 0;
}

// ── 修复引导（docs/15 · P5）──────────────────────────────────────
// 定位系统盘 → 找 ESP（GUID → mountvol → FAT 回退）→ bcdboot → 产物断言。
// 用于：还原后引导坏了、或从 PE 里救一台起不来的机器（不必重装）。
int RepairBoot(int disk, int part, std::string& msg) {
    // ① 目标系统盘
    PartitionInfo target;
    bool found = false;
    auto disks = EnumerateDisks();
    for (const auto& d : disks) {
        for (const auto& p : d.parts) {
            if (p.letter.empty())
                continue;
            if (disk > 0 || part > 0) {
                if ((int)d.index != disk || (int)p.partNumber != part)
                    continue;
            }
            std::wstring win = p.letter + L":\\Windows\\System32\\winload.exe";
            if (GetFileAttributesW(win.c_str()) == INVALID_FILE_ATTRIBUTES)
                continue;
            target = p;
            found = true;
            if (p.isSystem)
                break;  // 优先正在运行的系统盘
        }
        if (found && target.isSystem)
            break;
        if (found && (disk > 0 || part > 0))
            break;
    }
    if (!found) {
        msg = Tr("未找到系统盘（没有分区含 \\Windows\\System32\\winload.exe）。\n"
                 "若指定了 --disk/--part，请确认分区号正确且有盘符。\n当前分区表：\n") +
              W2U(DescribePartitions());
        LogError("repair-boot: target system partition not found");
        return 1;
    }
    std::wstring winDir = target.letter + L":\\Windows";
    LogInfo("repair-boot: target=" + W2U(target.letter) + ": disk=" +
            std::to_string(target.diskIndex) + " part=" +
            std::to_string(target.partNumber));
    std::string out;
    std::string blog;
    if (!IsUefiFirmware()) {
        // BIOS/MBR：bcdboot 把 bootmgr/BCD 写到系统分区自身
        int rc = RunProcess(SysToolPath(L"bcdboot.exe"),
                            winDir + L" /s " + target.letter + L": /f BIOS", out);
        LogInfo("repair-boot bcdboot (BIOS) rc=" + std::to_string(rc) + " / " + out);
        if (rc != 0) {
            msg = Tr("修复引导失败（bcdboot rc=") + std::to_string(rc) +
                  Tr("）：") + out;
            LogError(msg);
            return 1;
        }
        msg = Tr("引导已修复（BIOS）。");
        return 0;
    }
    // UEFI：找 ESP（含回退）
    std::wstring espRoot;
    bool guidMatched = false;
    std::string detail;
    if (!AcquireEspRoot(espRoot, guidMatched, blog)) {
        msg = Tr("未找到可用的 ESP 引导分区，无法修复引导。\n"
                 "常见原因：DiskGenius 重建分区时漏建 ESP，或把 ESP 标成了 Basic "
                 "Data（应改为 EFI System 类型）。\n当前分区表：\n") +
              W2U(DescribePartitions());
        LogError(msg);
        return 1;
    }
    if (!guidMatched)
        LogWarn("repair-boot: ESP partition type is not EFI System; used fallback");
    if (!TargetBcdTemplateOk(target.letter[0], detail)) {
        UnmountEsp(espRoot, blog);
        msg = Tr("系统盘缺少引导模板（") + detail +
              Tr("），无法生成引导。请换用完整系统镜像或修复该文件。");
        LogError(msg);
        return 1;
    }
    if (!EspHasRoomForBcdboot(espRoot, detail)) {
        UnmountEsp(espRoot, blog);
        msg = Tr("ESP 空间不足（") + detail +
              Tr("）：请先清理 \\EFI\\ZJRESTORE\\ 旧版残留，或扩大 ESP。");
        LogError(msg);
        return 1;
    }
    int rc = RunProcess(
        SysToolPath(L"bcdboot.exe"),
        winDir + L" /s " + std::wstring(1, espRoot[0]) + L": /f UEFI", out);
    LogInfo("repair-boot bcdboot (UEFI) rc=" + std::to_string(rc) + " / " + out);
    if (rc != 0) {
        UnmountEsp(espRoot, blog);
        msg = Tr("修复引导失败（bcdboot rc=") + std::to_string(rc) + Tr("）：") + out;
        LogError(msg);
        return 1;
    }
    if (!VerifyEspBcd(espRoot, detail)) {
        UnmountEsp(espRoot, blog);
        msg = Tr("引导仍不完整（校验未通过）：") + detail;
        LogError(msg);
        return 1;
    }
    // 打印 ESP BCD 的关键行，便于用户/我们确认（displayorder 是不是有值）
    {
        std::string enumOut;
        RunProcess(SysToolPath(L"bcdedit.exe"),
                   L"/store \"" + espRoot + L"EFI\\Microsoft\\Boot\\BCD\" /enum {bootmgr}",
                   enumOut);
        for (size_t pos = 0; pos < enumOut.size();) {
            size_t e = enumOut.find('\n', pos);
            std::string line = enumOut.substr(pos, e == std::string::npos ? e : e - pos);
            if (line.find("displayorder") != std::string::npos)
                LogInfo("repair-boot: " + line);
            if (e == std::string::npos)
                break;
            pos = e + 1;
        }
    }
    UnmountEsp(espRoot, blog);
    msg = Tr("引导已修复并校验通过（UEFI）。");
    return 0;
}

}  // namespace sysrecover
