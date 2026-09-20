// 进度文件实现：整文件覆写（小文件，原子性要求低）。
#include "progress.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace sysrecover {
namespace {

std::wstring g_path;

std::string JsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"')
            o += "\\\"";
        else if (c == '\\')
            o += "\\\\";
        else if (c == '\n')
            o += "\\n";
        else if ((unsigned char)c < 0x20)
            o += ' ';
        else
            o += c;
    }
    return o;
}

}  // namespace

void ProgressInit(const std::wstring& logsDir) {
    g_path = logsDir + L"\\progress.json";
}

void ProgressUpdate(const std::string& phase, int percent,
                    const std::string& status) {
    if (g_path.empty())
        return;
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"phase\":\"%s\",\"percent\":%d,\"status\":\"%s\",\"detail\":\""
             "\",\"updated_at\":\"%04d-%02d-%02d %02d:%02d:%02d\",\"pid\":%lu}",
             JsonEscape(phase).c_str(), percent, JsonEscape(status).c_str(),
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             GetCurrentProcessId());
    HANDLE h =
        CreateFileW(g_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(h, buf, (DWORD)strlen(buf), &written, nullptr);
    CloseHandle(h);
}

void ProgressDone(const std::string& phase, const std::string& status) {
    ProgressUpdate(phase, 100, status);
}

}  // namespace sysrecover
