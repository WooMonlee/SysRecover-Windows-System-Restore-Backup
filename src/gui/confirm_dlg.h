#pragma once
// 通用确认框（Ask2 二选 / Ask3 三选）。
//
// 还原确认（2026-10-14 用户规格）：需重启时弹三选 ——
//   取消（ESC/回车安全项）/ 还原后重启（swap + keepOld=true，可返回旧系统）/
//   重启后还原（block，C 盘内容全丢）；defaultIdx=1（回车=还原后重启）。
// 就地还原保持二选：取消 / 开始还原。
// 静默模式（勾选）下完全不弹框，engine=swap + keepOld=false。
//
// 注意：所有 C++ 标准头必须在 StdAfx.h 之前（PIT-012）。
#include <string>

#include "StdAfx.h"

using namespace DuiLib;

class CConfirmDlg : public WindowImplBase {
public:
    enum { kExit = 0, kExitReboot = 1 };

    // 通用二选：返回 0（左按钮）/ 1（右按钮）。owner 可为 nullptr。
    // leftText / rightText 为空则沿用 skin/confirm.xml 的静态文案。
    // defaultIsRight：回车落在哪个按钮（false = 左）。ESC 一律等同左按钮（安全项）。
    // hl：消息里要红+粗显示的子串（如「丢失！」/「保留！」），可空。
    static int Ask2(HWND owner, const std::wstring& title, const std::wstring& msg,
                    const std::wstring& leftText, const std::wstring& rightText,
                    bool defaultIsRight, const std::wstring& hl = std::wstring());

    // 通用三选（还原确认 取消/还原后重启/重启后还原、AskUndo 备返处理等）：
    // 返回 0（左）/ 1（中）/ 2（右）。
    // 三个按钮重排为等宽；defaultIdx 指定回车落在哪（0/1/2）；ESC 仍是 0（安全项）。
    // midDisabled（2026-10-20，用户规格）：中间按钮不可选 —— 显示为红色
    // 「空间不足」并忽略点击（A 引擎空间不足时用；调用方应同时把 defaultIdx
    // 指到安全项/右按钮）。
    static int Ask3(HWND owner, const std::wstring& title, const std::wstring& msg,
                    const std::wstring& leftText, const std::wstring& midText,
                    const std::wstring& rightText, int defaultIdx,
                    bool midDisabled = false);

    // 通用四选（2026-10-20，用户规格：删除旧系统后自删）。
    // 返回 0（左）/ 1（中）/ 2（右）/ 3（第 4 个）。
    // 窗口加宽到 520，四个按钮等宽重排；defaultIdx 0..3；ESC 仍是 0（安全项）。
    static int Ask4(HWND owner, const std::wstring& title, const std::wstring& msg,
                    const std::wstring& leftText, const std::wstring& midText,
                    const std::wstring& rightText, const std::wstring& moreText,
                    int defaultIdx);

    // 兼容旧调用（还原确认）：左=退出、右=还原后重启（默认为右）。
    // 弹出模态确认框并返回 kExit / kExitReboot。
    static int Ask(HWND owner, const std::wstring& title, const std::wstring& msg);

    virtual CDuiString GetSkinFolder() override;
    virtual CDuiString GetSkinFile() override;
    virtual LPCTSTR GetWindowClassName() const override;
    virtual void InitWindow() override;
    virtual void Notify(TNotifyUI& msg) override;
    virtual LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) override;
    virtual CControlUI* CreateControl(LPCTSTR pstrClass) override;

private:
    void Finish(int result);
    static int RunModal(HWND owner, CConfirmDlg* dlg);  // Ask2/Ask3 共用模态循环

    int          m_result = kExit;
    bool         m_alive = false;
    std::wstring m_title;
    std::wstring m_msg;
    std::wstring m_leftText;
    std::wstring m_rightText;
    std::wstring m_midText;   // 非空 = 三选模式（Ask3）
    std::wstring m_moreText;  // 非空 = 四选模式（Ask4）
    bool         m_midDisabled = false;  // Ask3 中间按钮禁用（红「空间不足」）
    std::wstring m_hl;        // 非空 = 消息里红+粗的子串（Ask2 专用）
    bool         m_defaultRight = true;
    int          m_defaultIdx = 1;  // 回车落点：二选 0/1、三/四选 0..2/0..3
};
