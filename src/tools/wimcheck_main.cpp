// SysRecoverCheck —— 独立镜像检查程序（需求 2，2026-10-27）。
//
// 与主程序「更多功能 → 镜像检查」同源（都走 WimEngine::Probe / ListImages /
// Verify），但**不依赖 GUI**：双击即可用，也支持命令行 / 把镜像拖到 exe 上。
//
// 设计取舍：
//  · 用 main() + CommandLineToArgvW 取宽参数 —— CRT 的 char** argv 走 ANSI
//    代码页，中文路径在非中文系统上会变问号；这里直接拿 UTF-16。
//  · 不装崩溃处理器 / 不做 x64 自举 / 不嵌提权清单 —— 检查镜像是**只读**操作，
//    不需要管理员。根目录 SysRecoverCheck.exe 是 x64 启动器（转调 x64\ 真程序），
//    32 位系统用同目录 SysRecoverCheck_x86.exe。
//  · 全量校验可能跑很久：Ctrl+C = 干净取消（rc=6），进度行带「已用」。
//  · 退出码与主 CLI 对齐：0 成功 / 1 失败 / 2 参数错误 / 5 校验失败 / 6 取消。
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "common/i18n.h"
#include "common/version.h"
#include "wim/wim.h"

namespace {

using sysrecover::Tr;

volatile LONG g_cancel = 0;
std::chrono::steady_clock::time_point g_start;

BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        InterlockedExchange(&g_cancel, 1);
        return TRUE;
    }
    return FALSE;
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr,
                                nullptr);
    std::string s(n > 0 ? n - 1 : 0, 0);
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr,
                            nullptr);
    return s;
}

// 取译文并转 UTF-8（本程序统一用 printf 输出 —— MinGW 的 wprintf 把 %s 当窄串，
// 见 PIT-007，所以全程窄串 + UTF-8 控制台代码页）。
std::string T(const wchar_t* key) {
    return ToUtf8(std::wstring(sysrecover::Tr(key)));
}

void Say(const wchar_t* key) { std::printf("%s\n", T(key).c_str()); }

std::string FmtElapsed() {
    auto s = std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::steady_clock::now() - g_start)
                 .count();
    char b[32];
    if (s >= 3600)
        snprintf(b, sizeof(b), "%ld:%02ld:%02ld", (long)(s / 3600),
                 (long)((s % 3600) / 60), (long)(s % 60));
    else
        snprintf(b, sizeof(b), "%02ld:%02ld", (long)(s / 60), (long)(s % 60));
    return b;
}

// wimlib 的阶段名是英文（"verify"），界面上要显示中文 —— 只映射本程序会出现
// 的那几个，未知阶段原样透出（不吞信息）。注意 wim.cpp 会在阶段名后追加
// 「速度 ETA」（如 "verify  447.4 MB/s  ETA 0:00"），所以按**前缀**替换。
std::string StageText(const std::string& stage) {
    const std::string cn = T(Tr(L"校验镜像"));
    if (stage == "verify")
        return cn;
    if (stage.compare(0, 6, "verify") == 0)
        return cn + stage.substr(6);
    return stage;
}

// 校验进度行：模板与主 CLI 同一个词条（键 = 中文源文本），不新增译文。
bool CheckProgress(int pct, const std::string& rawStage) {
    if (InterlockedCompareExchange(&g_cancel, 0, 0) != 0)
        return true;  // 用户 Ctrl+C → wimlib 中止（rc=76 → 退出码 6）
    const std::string stage = StageText(rawStage);
    static int last = -1;
    static std::string lastStage;
    static std::string lastElapsed;
    std::string el = FmtElapsed();
    if (pct == last && stage == lastStage && el == lastElapsed)
        return false;
    last = pct;
    lastStage = stage;
    lastElapsed = el;
    std::printf(sysrecover::Tr("\r  %3d%%  %s   已用 %s   "), pct,
                stage.c_str(), el.c_str());
    std::fflush(stdout);
    return false;
}

// 双击 / 把文件拖到 exe 上时，系统会为我们新建一个控制台（进程数 = 1）。
// 这种情况下跑完要停一下，否则窗口一闪就关，用户什么都看不到。
//
// 但从根目录的 SysRecoverCheck.exe（x64 启动器）进来时，控制台归**启动器**
// 所有（它在等我们退出），GetConsoleProcessList() 会返回 2 —— 直接判"不是
// 自己的控制台"会既弹不出文件框、也来不及显示结果（控制台随启动器一起消失）。
// 启动器在这种情况下留环境变量 ZJ_CHECK_LAUNCHER=1 告诉我们"这个控制台是你的"；
// 脚本/批处理调用时启动器不会留标记，仍然不弹框、不等待。
bool OwnConsole() {
    wchar_t flag[8] = {};
    const DWORD fn = GetEnvironmentVariableW(L"ZJ_CHECK_LAUNCHER", flag, 8);
    if (fn == 1 && flag[0] == L'1')
        return true;
    DWORD pids[4] = {};
    DWORD n = GetConsoleProcessList(pids, 4);
    return n > 0 && n <= 1;
}

void PauseIfOwnConsole() {
    if (!OwnConsole())
        return;
    std::printf("\n%s", T(Tr(L"按回车键退出...")).c_str());
    std::fflush(stdout);
    std::getchar();
}

unsigned long long FileSizeOf(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        return 0;
    return (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) |
           fad.nFileSizeLow;
}

std::wstring FormatTime(unsigned long long ft64) {
    if (ft64 == 0)
        return L"-";
    FILETIME ft, lft;
    SYSTEMTIME st;
    ft.dwLowDateTime = (DWORD)(ft64 & 0xFFFFFFFFull);
    ft.dwHighDateTime = (DWORD)(ft64 >> 32);
    if (!FileTimeToLocalFileTime(&ft, &lft) ||
        !FileTimeToSystemTime(&lft, &st))
        return L"-";
    wchar_t b[40];
    swprintf(b, 40, L"%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute);
    return b;
}

// 多选打开框：选中多个文件时 GetOpenFileNameW 返回「目录\0文件1\0文件2\0\0」。
bool PickImages(std::vector<std::wstring>& out) {
    std::vector<wchar_t> buf(65536, 0);  // 多选要远大于 MAX_PATH
    std::wstring filter;
    filter += sysrecover::Tr(L"系统镜像");
    filter += L" (*.wim;*.esd)";
    filter.push_back(L'\0');
    filter += L"*.wim;*.esd";
    filter.push_back(L'\0');
    filter += sysrecover::Tr(L"所有文件");
    filter += L" (*.*)";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');
    filter.push_back(L'\0');

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetConsoleWindow();
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle = sysrecover::Tr(L"选择要检查的系统镜像");
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST |
                OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn))
        return false;

    const wchar_t* p = buf.data();
    std::wstring first = p;
    if (first.empty())
        return false;
    p += first.size() + 1;
    if (*p == L'\0') {
        out.push_back(first);  // 只选了一个：直接是完整路径
        return true;
    }
    std::wstring dir = first;  // 多个：第一段是目录
    if (dir.back() != L'\\' && dir.back() != L'/')
        dir += L'\\';
    while (*p) {
        std::wstring name = p;
        out.push_back(dir + name);
        p += name.size() + 1;
    }
    return !out.empty();
}

void PrintUsage() {
    Say(Tr(L"用法："));
    std::printf("  SysRecoverCheck.exe [%s] <%s> [<%s> ...]\n\n",
                T(Tr(L"选项")).c_str(), T(Tr(L"镜像文件")).c_str(),
                T(Tr(L"镜像文件")).c_str());
    Say(Tr(L"选项："));
    std::printf("  --quick       %s\n", T(Tr(L"只做可用性快检并列出子镜像")).c_str());
    std::printf("  --lang zh|en  %s\n", T(Tr(L"界面语言")).c_str());
    std::printf("  --help, -h    %s\n\n", T(Tr(L"显示本帮助")).c_str());
    Say(Tr(L"不带参数时会弹出文件选择框（可多选）；也可以把镜像文件拖到本程序上。"));
    Say(Tr(L"退出码：0 成功 / 1 失败 / 2 参数错误 / 5 校验失败 / 6 取消"));
}

// 单个镜像：快检 → 列子镜像 →（除非 --quick）全量校验。
// 返回该文件的退出码（0 成功 / 1 失败 / 5 校验失败 / 6 取消）。
int CheckOne(const std::wstring& path, bool quick) {
    std::printf("\n%s: %s\n", T(Tr(L"镜像")).c_str(), ToUtf8(path).c_str());
    unsigned long long size = FileSizeOf(path);
    if (size == 0) {
        std::printf("  %s: %s\n", T(Tr(L"快检未通过")).c_str(),
                    T(Tr(L"文件不存在或无法访问")).c_str());
        return 1;
    }
    std::printf("  %s: %.2f GB\n", T(Tr(L"文件大小")).c_str(),
                size / 1073741824.0);

    sysrecover::WimEngine engine;
    if (!engine.ok()) {
        std::printf("  %s: %s\n", T(Tr(L"快检未通过")).c_str(),
                    T(Tr(L"wimlib 初始化失败")).c_str());
        return 1;
    }

    std::wstring why;
    int rc = engine.Probe(path, why);
    if (rc != 0) {
        std::printf("  %s: %s", T(Tr(L"快检未通过")).c_str(),
                    ToUtf8(why).c_str());
        if (why.empty())
            std::printf("(%s)", ToUtf8(sysrecover::WimEngine::ErrorString(rc))
                                    .c_str());
        std::printf("\n");
        return 1;
    }
    std::printf("  %s: %s\n", T(Tr(L"快检")).c_str(), T(Tr(L"通过")).c_str());

    std::vector<sysrecover::ImageDesc> list;
    if (engine.ListImages(path, list) == 0 && !list.empty()) {
        std::printf("  %s:\n", T(Tr(L"子镜像")).c_str());
        for (const auto& img : list) {
            std::printf("    %d | %s | %.2f GB | %s\n", img.index,
                        ToUtf8(img.name).c_str(),
                        img.sizeBytes / 1073741824.0,
                        ToUtf8(FormatTime(img.creationTime)).c_str());
            if (!img.description.empty())
                std::printf("        %s\n", ToUtf8(img.description).c_str());
        }
    }

    if (quick) {
        std::printf("  %s: %s\n", T(Tr(L"状态")).c_str(),
                    T(Tr(L"快检通过（未校验文件数据）")).c_str());
        return 0;
    }

    std::printf("  %s:\n", T(Tr(L"校验文件数据")).c_str());
    g_start = std::chrono::steady_clock::now();
    const std::string stage = T(Tr(L"校验镜像"));
    CheckProgress(0, stage);
    rc = engine.Verify(path, CheckProgress);
    std::printf("\n");
    if (rc == 0) {
        std::printf("  %s: %s\n", T(Tr(L"校验")).c_str(), T(Tr(L"通过")).c_str());
        return 0;
    }
    if (InterlockedCompareExchange(&g_cancel, 0, 0) != 0) {
        std::printf("  %s: %s\n", T(Tr(L"校验")).c_str(), T(Tr(L"已取消")).c_str());
        return 6;
    }
    std::printf("  %s: %s (%s)\n", T(Tr(L"校验")).c_str(), T(Tr(L"失败")).c_str(),
                ToUtf8(sysrecover::WimEngine::ErrorString(rc)).c_str());
    return 5;
}

}  // namespace

int main() {
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    struct CpGuard {
        UINT out;
        ~CpGuard() { SetConsoleOutputCP(out); }
    } cpGuard{GetConsoleOutputCP()};
    SetConsoleOutputCP(CP_UTF8);

    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i)
        args.push_back(wargv ? wargv[i] : L"");
    if (wargv)
        LocalFree(wargv);

    bool quick = false;
    bool help = false;
    std::vector<std::wstring> files;
    std::string langArg;
    std::wstring badArg;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        if (a == L"--quick") {
            quick = true;
        } else if (a == L"--help" || a == L"-h" || a == L"/?") {
            help = true;
        } else if (a == L"--lang" && i + 1 < args.size()) {
            langArg = ToUtf8(args[++i]);
        } else if (a.rfind(L"--lang=", 0) == 0) {
            langArg = ToUtf8(a.substr(7));
        } else if (a.size() >= 2 && a[0] == L'-') {
            badArg = a;
        } else {
            files.push_back(a);
        }
    }

    sysrecover::InitI18n(langArg.empty() ? nullptr : langArg.c_str());

    std::printf("SysRecover \xC2\xB7 %s %s\n", T(Tr(L"镜像检查")).c_str(),
                SYSRECOVER_VERSION);

    if (!badArg.empty()) {
        std::printf("\n%s: %s\n", T(Tr(L"未知选项")).c_str(),
                    ToUtf8(badArg).c_str());
        PrintUsage();
        PauseIfOwnConsole();
        return 2;
    }
    if (help) {
        PrintUsage();
        PauseIfOwnConsole();
        return 0;
    }
    if (files.empty()) {
        // 命令行里没给文件：双击 / 拖拽场景弹选择框；脚本场景直接给用法。
        if (!OwnConsole()) {
            PrintUsage();
            return 2;
        }
        if (!PickImages(files) || files.empty()) {
            Say(Tr(L"没有选择镜像，已取消。"));
            return 6;
        }
    }

    int worst = 0;
    for (const auto& f : files) {
        int rc = CheckOne(f, quick);
        if (rc == 6) {  // 用户取消：后面的不再处理
            worst = 6;
            break;
        }
        if (rc == 5)
            worst = 5;
        else if (rc != 0 && worst == 0)
            worst = 1;
    }

    std::printf("\n");
    switch (worst) {
        case 0:
            Say(Tr(L"检查完成：全部通过。"));
            break;
        case 5:
            Say(Tr(L"检查完成：有镜像校验失败（文件数据损坏）。"));
            break;
        case 6:
            Say(Tr(L"检查已取消。"));
            break;
        default:
            Say(Tr(L"检查完成：有镜像未通过快检。"));
            break;
    }
    PauseIfOwnConsole();
    return worst;
}
