#pragma once
// 加密镜像口令框（docs/30）：选中 .zje 后输入口令 / 备份时设置口令。
// 只把口令留在内存（imgsrc 的进程表），不写日志、契约、history。
#include <string>

#include "StdAfx.h"

using namespace DuiLib;

class CPasswordDlg : public WindowImplBase {
public:
    // 返回 true = 用户点「确定」（pwd 非空）；false = 取消/关闭。
    static bool Ask(HWND owner, const std::wstring& title,
                    const std::wstring& hint, std::wstring& pwd,
                    const std::wstring& okText = std::wstring());

    virtual CDuiString GetSkinFolder() override;
    virtual CDuiString GetSkinFile() override;
    virtual LPCTSTR GetWindowClassName() const override;
    virtual CControlUI* CreateControl(LPCTSTR pstrClass) override;
    virtual void InitWindow() override;
    virtual void Notify(TNotifyUI& msg) override;
    virtual LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) override;

private:
    void Finish(bool ok);
    // 原生 EDIT 子窗口是**延迟创建**的（CSkinEditUI::SetPos 里建），InitWindow
    // 时可能还没有 → 密码掩码/回车转发要在首次激活时补做。
    void EnsureEditSetup();
    std::wstring ReadEdit();

    std::wstring m_title;
    std::wstring m_hint;
    std::wstring m_okText;
    std::wstring m_value;
    bool m_result = false;
    bool m_alive = false;
    bool m_editSetup = false;
};
