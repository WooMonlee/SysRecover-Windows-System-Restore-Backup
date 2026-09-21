#pragma once
// 通用二选确认框：主窗口在还原前弹出，按钮为「退出」/「退出并重启」。
//
// 用户规格（2026-09-16）：非静默模式下，还原流程只保留这一个选择框 ——
// 用户选定「退出并重启」后暂存任务并自动重启，不再有额外的成功提示框，
// 也不弹 Windows 关机通知。静默模式（勾选）下完全不弹框。
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
    static int Ask2(HWND owner, const std::wstring& title, const std::wstring& msg,
                    const std::wstring& leftText, const std::wstring& rightText,
                    bool defaultIsRight);

    // 兼容旧调用（还原确认）：左=退出不重启、右=重启后还原（默认为右）。
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

    int          m_result = kExit;
    bool         m_alive = false;
    std::wstring m_title;
    std::wstring m_msg;
    std::wstring m_leftText;
    std::wstring m_rightText;
    bool         m_defaultRight = true;
};
