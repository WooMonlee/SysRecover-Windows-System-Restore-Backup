// 快捷方式实现。
#include "shortcut.h"

#include <objbase.h>
#include <shobjidl.h>
#include <windows.h>

namespace sysrecover {

bool CreateShortcut(const std::wstring& folder, const std::wstring& name,
                    const std::wstring& target,
                    const std::wstring& targetArgs, std::wstring& outPath) {
    outPath.clear();
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool uninit = SUCCEEDED(hr);
    bool ok = false;
    do {
        IShellLinkW* link = nullptr;
        hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IShellLinkW,
                              reinterpret_cast<void**>(&link));
        if (FAILED(hr) || !link)
            break;
        link->SetPath(target.c_str());
        if (!targetArgs.empty())
            link->SetArguments(targetArgs.c_str());
        wchar_t dir[MAX_PATH] = {};
        wcsncpy_s(dir, target.c_str(), _TRUNCATE);
        wchar_t* slash = wcsrchr(dir, L'\\');
        if (slash)
            *slash = 0;
        link->SetWorkingDirectory(dir);
        IPersistFile* file = nullptr;
        hr = link->QueryInterface(IID_IPersistFile,
                                  reinterpret_cast<void**>(&file));
        link->Release();
        if (FAILED(hr) || !file)
            break;
        outPath = folder + L"\\" + name + L".lnk";
        hr = file->Save(outPath.c_str(), TRUE);
        file->Release();
        ok = SUCCEEDED(hr);
        if (!ok)
            outPath.clear();
    } while (0);
    if (uninit)
        CoUninitialize();
    return ok;
}

}  // namespace sysrecover
