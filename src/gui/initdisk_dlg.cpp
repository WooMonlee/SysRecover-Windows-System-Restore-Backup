// 新硬盘分区向导（纯 Win32；见 initdisk_dlg.h）。所有文案走 Tr（i18n）。
// 2026-10-21 用户 VM 反馈：窗口加高 30px；小容量磁盘按规则推荐（Win10/11
// 可用<65GB → 整盘做系统盘；Win7 60GB → 30GB 系统 + 其余数据）。
#include "common/i18n.h"
#include "initdisk_dlg.h"

#include <cstdio>
#include <cstdlib>

using sysrecover::DiskScheme;
using sysrecover::InitDiskRequest;
using sysrecover::InitDiskResult;
using sysrecover::PartSizeHint;

namespace {

const wchar_t* kClass = L"SysRecoverInitDisk";
const int kIdGpt = 1001, kIdMbr = 1002, kIdSize = 1003, kIdWarn = 1006;
const int kIdHint = 1007;

struct Ctx {
    InitDiskRequest req;
    InitDiskResult res;
    bool done = false;
    HWND gpt = nullptr, mbr = nullptr, edit = nullptr, warn = nullptr;
    HWND hint = nullptr, espInfo = nullptr;
    HFONT font = nullptr;
    PartSizeHint gptHint, mbrHint;
};

std::wstring SizeText(unsigned long long bytes) {
    wchar_t b[64] = {};
    swprintf(b, 64, L"%.1f GB", bytes / 1073741824.0);
    return b;
}

void SetAllFont(Ctx* c, HWND hwnd) {
    EnumChildWindows(
        hwnd,
        [](HWND ch, LPARAM lp) -> BOOL {
            SendMessageW(ch, WM_SETFONT, (WPARAM)lp, TRUE);
            return TRUE;
        },
        (LPARAM)c->font);
}

DiskScheme SelectedScheme(Ctx* c) {
    return (SendMessageW(c->gpt, BM_GETCHECK, 0, 0) == BST_CHECKED)
               ? DiskScheme::Gpt
               : DiskScheme::Mbr;
}

unsigned long long EditValue(Ctx* c) {
    wchar_t buf[32] = {};
    GetWindowTextW(c->edit, buf, 32);
    return wcstoul(buf, nullptr, 10);
}

PartSizeHint& HintOf(Ctx* c, DiskScheme s) {
    return s == DiskScheme::Gpt ? c->gptHint : c->mbrHint;
}

std::wstring WarningText(Ctx* c) {
    const bool gpt = SelectedScheme(c) == DiskScheme::Gpt;
    std::wstring w = sysrecover::Tr(L"注意：将清除该磁盘上的全部分区与数据！");
    if (gpt && !c->req.uefiSession) {
        w += L"\n";
        w += sysrecover::Tr(L"当前恢复环境以 Legacy 方式启动：创建 GPT 盘后，"
                            L"请在 BIOS 中改为 UEFI 启动，否则系统无法引导；"
                            L"若启动项缺失，可用「修复引导」。");
    } else if (!gpt && c->req.uefiSession) {
        w += L"\n";
        w += sysrecover::Tr(L"当前恢复环境以 UEFI 方式启动：创建 MBR 盘后，"
                            L"请在 BIOS 中改为 Legacy/CSM 启动。");
    }
    return w;
}

void UpdateWarn(Ctx* c) {
    if (!c->warn) return;
    SetWindowTextW(c->warn, WarningText(c).c_str());
    InvalidateRect(c->warn, nullptr, TRUE);
}

void UpdateHint(Ctx* c) {
    if (!c->hint) return;
    const PartSizeHint& h = HintOf(c, SelectedScheme(c));
    unsigned long long v = EditValue(c);
    wchar_t b[256] = {};
    if (v == 0 || v > h.usableGb) {
        swprintf(b, 256,
                 sysrecover::Tr(L"将创建：系统盘 %lluGB（整盘，不创建数据盘）"),
                 h.systemGb);
    } else if (h.usableGb - v >= 5) {
        swprintf(b, 256,
                 sysrecover::Tr(L"将创建：系统盘 %lluGB + 数据盘约 %lluGB"), v,
                 h.usableGb - v);
    } else {
        swprintf(b, 256,
                 sysrecover::Tr(L"将创建：系统盘 %lluGB（剩余不足 5GB，"
                                L"不创建数据盘）"),
                 v);
    }
    SetWindowTextW(c->hint, b);
    if (c->espInfo) {
        SetWindowTextW(c->espInfo,
                       SelectedScheme(c) == DiskScheme::Gpt
                           ? sysrecover::Tr(L"ESP 300MB / MSR 16MB（自动创建）")
                           : sysrecover::Tr(L"MBR 方案：不创建 ESP / MSR"));
    }
}

// 切换方案时把系统盘大小恢复为该方案的推荐值（用户仍可再改）。
void ApplySchemeDefaults(Ctx* c) {
    const PartSizeHint& h = HintOf(c, SelectedScheme(c));
    wchar_t b[32] = {};
    swprintf(b, 32, L"%llu", h.systemGb);
    SetWindowTextW(c->edit, b);
    UpdateHint(c);
    UpdateWarn(c);
}

void Finish(Ctx* c, HWND hwnd, bool ok) {
    c->res.ok = ok;
    if (ok) {
        c->res.scheme = SelectedScheme(c);
        c->res.systemGb = EditValue(c);
    }
    c->done = true;
    DestroyWindow(hwnd);
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* c = reinterpret_cast<Ctx*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            c = reinterpret_cast<Ctx*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)c);
            c->font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH | FF_DONTCARE,
                                  L"Microsoft YaHei UI");
            c->gptHint = sysrecover::SuggestPartitionSizes(
                c->req.win7Family, DiskScheme::Gpt, c->req.disk.sizeBytes);
            c->mbrHint = sysrecover::SuggestPartitionSizes(
                c->req.win7Family, DiskScheme::Mbr, c->req.disk.sizeBytes);
            // 标题/磁盘信息
            wchar_t info[512] = {};
            swprintf(info, 512,
                     sysrecover::Tr(L"磁盘 %u：%ls%ls\n容量：%ls"),
                     c->req.disk.index,
                     c->req.disk.model.empty() ? sysrecover::Tr(L"本地磁盘")
                                               : c->req.disk.model.c_str(),
                     c->req.disk.serial.empty()
                         ? L""
                         : (std::wstring(L" (") + c->req.disk.serial + L")")
                               .c_str(),
                     SizeText(c->req.disk.sizeBytes).c_str());
            CreateWindowW(L"STATIC", sysrecover::Tr(L"为新硬盘创建分区"),
                          WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 14, 500, 26, hwnd,
                          nullptr, cs->hInstance, nullptr);
            CreateWindowW(L"STATIC", info,
                          WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 44, 520, 44, hwnd,
                          nullptr, cs->hInstance, nullptr);
            // 方案一（GPT）
            wchar_t opt1[512] = {};
            if (c->gptHint.createData)
                swprintf(opt1, 512,
                         sysrecover::Tr(L"方案一：GPT（UEFI 启动）— ESP 300MB + "
                                        L"MSR 16MB + 系统盘 %lluGB + 其余为数据盘"),
                         c->gptHint.systemGb);
            else
                swprintf(opt1, 512,
                         sysrecover::Tr(L"方案一：GPT（UEFI 启动）— ESP 300MB + "
                                        L"MSR 16MB + 系统盘 %lluGB（整盘，"
                                        L"不创建数据盘）"),
                         c->gptHint.systemGb);
            c->gpt = CreateWindowW(
                L"BUTTON", opt1,
                WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_MULTILINE,
                20, 104, 520, 40, hwnd, (HMENU)(INT_PTR)kIdGpt, cs->hInstance,
                nullptr);
            // 方案二（MBR）
            wchar_t opt2[512] = {};
            if (c->mbrHint.createData)
                swprintf(opt2, 512,
                         sysrecover::Tr(L"方案二：MBR（Legacy 启动）— 系统盘 "
                                        L"%lluGB + 其余为数据盘"),
                         c->mbrHint.systemGb);
            else
                swprintf(opt2, 512,
                         sysrecover::Tr(L"方案二：MBR（Legacy 启动）— 系统盘 "
                                        L"%lluGB（整盘，不创建数据盘）"),
                         c->mbrHint.systemGb);
            c->mbr = CreateWindowW(
                L"BUTTON", opt2,
                WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_MULTILINE,
                20, 150, 520, 40, hwnd, (HMENU)(INT_PTR)kIdMbr, cs->hInstance,
                nullptr);
            CreateWindowW(L"STATIC", sysrecover::Tr(L"系统盘大小（GB）："),
                          WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 206, 130, 26,
                          hwnd, nullptr, cs->hInstance, nullptr);
            c->edit = CreateWindowW(
                L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER | ES_LEFT, 154,
                202, 90, 26, hwnd, (HMENU)(INT_PTR)kIdSize, cs->hInstance,
                nullptr);
            c->hint = CreateWindowW(L"STATIC", L"",
                                    WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 234,
                                    520, 26, hwnd, (HMENU)(INT_PTR)kIdHint,
                                    cs->hInstance, nullptr);
            c->espInfo = CreateWindowW(
                L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 258, 520,
                26, hwnd, nullptr, cs->hInstance, nullptr);
            c->warn = CreateWindowW(
                L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 288, 510, 68, hwnd,
                (HMENU)(INT_PTR)kIdWarn, cs->hInstance, nullptr);
            CreateWindowW(L"BUTTON", sysrecover::Tr(L"创建分区"),
                          WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 300, 360,
                          110, 32, hwnd, (HMENU)IDOK, cs->hInstance, nullptr);
            CreateWindowW(L"BUTTON", sysrecover::Tr(L"取消"),
                          WS_CHILD | WS_VISIBLE, 430, 360, 90, 32, hwnd,
                          (HMENU)IDCANCEL, cs->hInstance, nullptr);
            SetAllFont(c, hwnd);
            // 可选性：技术硬限制时禁用对应单选
            if (!c->req.gptAllowed)
                EnableWindow(c->gpt, FALSE);
            if (!c->req.mbrAllowed)
                EnableWindow(c->mbr, FALSE);
            const bool preferGpt = c->req.preferred == DiskScheme::Gpt;
            SendMessageW(c->gpt, BM_SETCHECK,
                         preferGpt && c->req.gptAllowed ? BST_CHECKED
                                                        : BST_UNCHECKED,
                         0);
            SendMessageW(c->mbr, BM_SETCHECK,
                         (!preferGpt || !c->req.gptAllowed) &&
                                 c->req.mbrAllowed
                             ? BST_CHECKED
                             : BST_UNCHECKED,
                         0);
            ApplySchemeDefaults(c);
            SetFocus(c->edit);
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == kIdGpt || id == kIdMbr) {
                ApplySchemeDefaults(c);
                return 0;
            }
            if (id == kIdSize && HIWORD(wp) == EN_CHANGE) {
                UpdateHint(c);
                return 0;
            }
            if (id == IDOK && c) {
                const unsigned long long v = EditValue(c);
                if (v == 0) {
                    MessageBoxW(hwnd, sysrecover::Tr(L"请输入系统盘大小（GB）"),
                                sysrecover::Tr(L"分区"), MB_OK | MB_ICONWARNING);
                    return 0;
                }
                const PartSizeHint& h = HintOf(c, SelectedScheme(c));
                if (v > h.usableGb) {
                    wchar_t q[256] = {};
                    swprintf(q, 256,
                             sysrecover::Tr(L"系统盘容量超过可用空间（约 "
                                            L"%lluGB）"),
                             h.usableGb);
                    MessageBoxW(hwnd, q, sysrecover::Tr(L"分区"),
                                MB_OK | MB_ICONWARNING);
                    return 0;
                }
                // 小盘规则：剩余 <5GB 时不创建数据盘（用户 2026-10-21）。
                const bool createData =
                    (h.usableGb > v) && (h.usableGb - v >= 5);
                unsigned long long minGb = c->req.minSystemGb;
                if (minGb > h.usableGb) minGb = h.usableGb;
                if (v < minGb) {
                    wchar_t q[256] = {};
                    swprintf(q, 256,
                             sysrecover::Tr(L"系统盘 %lluGB 低于建议值 "
                                            L"%lluGB，可能不够系统更新使用。"
                                            L"仍要继续吗？"),
                             v, minGb);
                    if (MessageBoxW(hwnd, q, sysrecover::Tr(L"分区"),
                                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) !=
                        IDYES)
                        return 0;
                }
                sysrecover::PartInitPlan plan;
                plan.diskIndex = c->req.disk.index;
                plan.scheme = SelectedScheme(c);
                plan.systemGb = v;
                plan.win7Family = c->req.win7Family;
                plan.createData = createData;
                std::wstring err;
                if (!sysrecover::ValidatePartInitPlan(plan,
                                                      c->req.disk.sizeBytes,
                                                      err)) {
                    MessageBoxW(hwnd, err.c_str(), sysrecover::Tr(L"分区"),
                                MB_OK | MB_ICONWARNING);
                    return 0;
                }
                Finish(c, hwnd, true);
                return 0;
            }
            if (id == IDCANCEL) {
                if (c) Finish(c, hwnd, false);
                return 0;
            }
            return 0;
        }
        case WM_CLOSE:
            if (c) Finish(c, hwnd, false);
            return 0;
        case WM_DESTROY:
            if (c) {
                c->done = true;
                if (c->font) DeleteObject(c->font);
                c->font = nullptr;
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

namespace sysrecover {

InitDiskResult ShowInitDiskDialog(HWND owner, const InitDiskRequest& req) {
    Ctx ctx;
    ctx.req = req;
    static bool reg = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!reg) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = Proc;
        wc.hInstance = inst;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        reg = true;
    }
    const int w = 560, h = 430;  // 2026-10-21：+30px（原 400 按钮被裁）
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOOLWINDOW, kClass,
        Tr(L"为新硬盘创建分区"), WS_POPUP | WS_CAPTION | WS_SYSMENU,
        (sw - w) / 2, (sh - h) / 2, w, h, owner, nullptr, inst, &ctx);
    if (!hwnd) return ctx.res;
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    MSG m;
    while (!ctx.done && GetMessageW(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    if (owner) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    return ctx.res;
}

}  // namespace sysrecover
