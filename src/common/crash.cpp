#include "crash.h"

#include <windows.h>
#include <dbghelp.h>   // 只用类型（MINIDUMP_*）；函数动态加载，不链接 dbghelp
#include <tlhelp32.h>  // FindProcessIdsByName（Toolhelp 快照）

#include <cstdio>
#include <string>
#include <vector>

#include "../common/version.h"

namespace sysrecover {
namespace {

std::wstring g_crashDir;

std::string W2U(const std::wstring& w) {
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr,
                                0, nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr,
                        nullptr);
    return s;
}

void AppendFile(const std::wstring& path, const std::string& text) {
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD w = 0;
    WriteFile(h, text.data(), (DWORD)text.size(), &w, nullptr);
    CloseHandle(h);
}

// 异常码 → 可读名（够用即可；未列的显示十六进制）。
const char* ExceptionName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
        case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
        default: return "(unknown)";
    }
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    static LONG busy = 0;  // 防重入（崩溃处理里再崩就直接放行）
    if (InterlockedExchange(&busy, 1) != 0)
        return EXCEPTION_EXECUTE_HANDLER;

    const DWORD code = ep && ep->ExceptionRecord
                           ? ep->ExceptionRecord->ExceptionCode : 0;
    const void* addr = ep && ep->ExceptionRecord
                           ? ep->ExceptionRecord->ExceptionAddress : nullptr;

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stem[96];
    // 注意：这里不能用 %s 拼宽串（PIT-007）；纯数字用 %04u 没问题。
    swprintf(stem, 96, L"crash-%04u%02u%02u-%02u%02u%02u-%lu", st.wYear,
             st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             (unsigned long)GetCurrentProcessId());
    std::wstring base = g_crashDir + L"\\" + stem;

    // ── 1) 可读文本（先写它：dump 失败也至少有线索）──
    std::string txt;
    char line[1024];
    snprintf(line, sizeof(line), "SysRecover crash report\n");
    txt += line;
    snprintf(line, sizeof(line), "version : %s\n", SYSRECOVER_VERSION);
    txt += line;
    snprintf(line, sizeof(line), "time    : %04u-%02u-%02u %02u:%02u:%02u\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    txt += line;
    snprintf(line, sizeof(line), "code    : 0x%08lX (%s)\n", (unsigned long)code,
             ExceptionName(code));
    txt += line;
    snprintf(line, sizeof(line), "addr    : 0x%p\n", addr);
    txt += line;
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord &&
        ep->ExceptionRecord->NumberParameters >= 2) {
        const ULONG_PTR* p = ep->ExceptionRecord->ExceptionInformation;
        snprintf(line, sizeof(line),
                 "av      : %s address 0x%p\n",
                 p[0] == 0 ? "read" : (p[0] == 1 ? "write" : "execute"),
                 (void*)p[1]);
        txt += line;
    }

    // 调用栈地址 + 所在模块（无符号表也能定位到"哪个 DLL/什么偏移"）
    void* frames[48] = {};
    USHORT nf = CaptureStackBackTrace(0, 48, frames, nullptr);
    txt += "stack   :\n";
    for (USHORT i = 0; i < nf; ++i) {
        HMODULE mod = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)frames[i], &mod) &&
            mod) {
            wchar_t mp[MAX_PATH] = {};
            GetModuleFileNameW(mod, mp, MAX_PATH);
            snprintf(line, sizeof(line), "  [%02u] 0x%p  %s+0x%llX\n", i,
                     frames[i], W2U(mp).c_str(),
                     (unsigned long long)((ULONG_PTR)frames[i] -
                                          (ULONG_PTR)mod));
        } else {
            snprintf(line, sizeof(line), "  [%02u] 0x%p\n", i, frames[i]);
        }
        txt += line;
    }
    txt += "cmdline : " + W2U(GetCommandLineW()) + "\n";
    AppendFile(base + L".txt", txt);
    AppendFile(g_crashDir + L"\\last.txt", txt);

    // ── 2) minidump（动态加载 dbghelp，不引入链接依赖）──
    HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
    if (dbg) {
        typedef BOOL(WINAPI * Fn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                  PMINIDUMP_EXCEPTION_INFORMATION,
                                  PMINIDUMP_USER_STREAM_INFORMATION,
                                  PMINIDUMP_CALLBACK_INFORMATION);
        Fn writeDump =
            (Fn)(void*)GetProcAddress(dbg, "MiniDumpWriteDump");
        if (writeDump) {
            HANDLE f = CreateFileW((base + L".dmp").c_str(), GENERIC_WRITE, 0,
                                   nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION mei = {};
                mei.ThreadId = GetCurrentThreadId();
                mei.ExceptionPointers = ep;
                mei.ClientPointers = FALSE;
                writeDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                          MiniDumpNormal, &mei, nullptr, nullptr);
                CloseHandle(f);
            }
        }
        FreeLibrary(dbg);
    }
    return EXCEPTION_EXECUTE_HANDLER;  // 处理完让它正常终止
}

}  // namespace

std::wstring CrashDir(const std::wstring& baseDir) {
    return baseDir + L"\\logs\\crash";
}

// ── 「日志」按钮用（支持包）：给别的进程写迷你转储 ─────────────────────────
// MiniDumpNormal 已含**全部线程的调用栈**（足够看等待链/死锁卡在哪个模块），
// 再加 WithThreadInfo 拿线程时间戳；比 FullMemory 小一个数量级，便于发给支持。
bool WriteProcessMiniDump(unsigned long pid, const std::wstring& dmpPath) {
    HANDLE ph = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE,
                            (DWORD)pid);
    if (!ph)
        return false;
    HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
    if (!dbg) {
        CloseHandle(ph);
        return false;
    }
    typedef BOOL(WINAPI * Fn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                              PMINIDUMP_EXCEPTION_INFORMATION,
                              PMINIDUMP_USER_STREAM_INFORMATION,
                              PMINIDUMP_CALLBACK_INFORMATION);
    Fn writeDump = (Fn)(void*)GetProcAddress(dbg, "MiniDumpWriteDump");
    bool ok = false;
    if (writeDump) {
        HANDLE f = CreateFileW(dmpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            ok = writeDump(ph, (DWORD)pid, f,
                           (MINIDUMP_TYPE)(MiniDumpNormal |
                                           MiniDumpWithThreadInfo),
                           nullptr, nullptr, nullptr) != FALSE;
            CloseHandle(f);
            if (!ok)
                DeleteFileW(dmpPath.c_str());
        }
    }
    FreeLibrary(dbg);
    CloseHandle(ph);
    return ok;
}

std::vector<unsigned long> FindProcessIdsByName(const wchar_t* exeName) {
    std::vector<unsigned long> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return out;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0)
                out.push_back(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

void InstallCrashHandler(const std::wstring& baseDir) {
    g_crashDir = CrashDir(baseDir);
    // 建目录（失败就静默；崩溃处理不能把程序拖垮）
    std::wstring cur;
    for (wchar_t c : g_crashDir) {
        cur += c;
        if (c == L'\\' && cur.size() > 3)
            CreateDirectoryW(cur.c_str(), nullptr);
    }
    CreateDirectoryW(g_crashDir.c_str(), nullptr);
    SetUnhandledExceptionFilter(CrashFilter);
}

}  // namespace sysrecover
