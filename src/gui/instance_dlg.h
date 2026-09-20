#pragma once
// 「程序已在运行」三选对话框（单实例互斥命中时由 main_win.cpp 弹出）。
//
// 用户规格：已有本程序在运行时，让用户选择
//   ① 使用之前的程序（激活已有窗口并退出当前程序）—— **默认项**
//   ② 使用现在的程序（关掉之前的，用现在这个）
//   ③ 直接退出
//
// 关键安全约束：旧实例正在跑备份/还原时（m_busy），②必须不可用 —— 强关会
// 在写盘中途杀掉 worker 线程，留下半成品镜像或中断的还原暂存（AGENTS.md §11
// 安全红线）。所以在弹框前先跨进程查一次忙碌状态，忙则禁用②。
//
// 注意：所有 C++ 标准头必须在 StdAfx.h 之前（PIT-012）。
#include <string>

#include "StdAfx.h"

using namespace DuiLib;

class CInstanceDlg : public WindowImplBase {
public:
    enum { kUsePrevious = 0, kUseCurrent = 1, kQuit = 2 };

    // 弹出对话框（模态，自带局部消息循环）并返回上面三个值之一。
    // prevBusy = 旧实例是否正在执行任务（true 时禁用「使用现在的程序」）。
    static int Ask(bool prevBusy);

    virtual CDuiString GetSkinFolder() override;
    virtual CDuiString GetSkinFile() override;
    virtual LPCTSTR GetWindowClassName() const override;
    virtual void InitWindow() override;
    virtual void Notify(TNotifyUI& msg) override;
    virtual LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) override;
    virtual CControlUI* CreateControl(LPCTSTR pstrClass) override;

private:
    void Finish(int result);

    int  m_result = kUsePrevious;  // 回车 = 默认项（用户要求默认第一个）
    bool m_prevBusy = false;
    bool m_alive = false;          // 局部消息循环的存活标志
};
