#pragma once
// 主窗口（经典 Duilib WindowImplBase）。
// 关键：所有 C++ 标准头必须在 StdAfx.h 之前（PIT-012②：min/max 宏与
// libstdc++ <algorithm>/<functional> 模板冲突）。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "StdAfx.h"

using namespace DuiLib;

// 自定义 WM_APP 消息：worker 线程 → 主线程 UI 更新
#define WM_PROGRESS_UPDATE (WM_APP + 1)   // wParam=pct, lParam=ptr to std::wstring
#define WM_TASK_COMPLETE   (WM_APP + 2)   // wParam=rc (0=成功)
// 跨进程查询：返回 1 = 正在执行备份/还原任务（拒绝被关闭），0 = 空闲。
// 由新启动的实例发给已有实例（main_win.cpp 的单实例流程），同权限下可通。
#define WM_SR_QUERY_BUSY   (WM_APP + 3)

namespace sysrecover { struct PartitionInfo; }

class CMainForm : public WindowImplBase {
public:
    CMainForm();
    virtual ~CMainForm() override;

    virtual CDuiString GetSkinFolder() override;
    virtual CDuiString GetSkinFile() override;
    virtual LPCTSTR GetWindowClassName(void) const override;
    virtual void InitWindow() override;
    virtual void Notify(TNotifyUI& msg) override;
    virtual LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) override;
    // 自绘控件注入点（未知 UIClass 回落到这里，见 ui_skin.h 头部注释）
    virtual CControlUI* CreateControl(LPCTSTR pstrClass) override;

private:
    void PopulatePartitions();
    void BrowseWimFile();
    void BrowseSaveFile();
    void LoadWimImages(const std::wstring& path);
    void StartRestore();   // UI 校验 + 确认 → StartRestoreAsync
    void StartBackup();    // UI 校验 + 确认 → StartBackupAsync
    void StartRestoreAsync();  // worker 线程：StageRestore
    void StartBackupAsync();   // worker 线程：RunBackup
    void ApplyModeUi();
    void UpdateMainAction();
    void ToggleBootMenu();       // 「安装/删除启动还原」按钮
    void RefreshBootMenuBtn();   // 按是否已安装刷新按钮文字
    void RebootNow();
    bool IsSilent();   // 「静默模式」勾选态
    void SetStatus(const std::wstring& text);
    void SetProgress(int pct);
    void OnProgressUpdate(int pct, const std::wstring& stage);
    void OnTaskComplete(int rc);
    int  BackupFmt() const;  // combo 0/1/2
    std::wstring ElapsedText() const;  // 已用 mm:ss / h:mm:ss

    bool m_backupMode = false;
    bool m_needReboot = true;  // 还原：true=已暂存需重启；false=已就地完成（PIT-064）
    bool m_imageOk = false;    // 还原模式：当前镜像可读/可用（否则主按钮保持灰）
    std::wstring m_wimPath;
    int m_selPart = -1;
    int m_selImageIndex = 1;
    std::vector<int> m_imgIdx;
    std::vector<sysrecover::PartitionInfo> m_parts;

    // worker 线程
    std::thread       m_worker;
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_busy{false};
    int               m_lastRc = 0;
    std::wstring      m_workerSource;   // StartBackupAsync 用：源路径
    std::wstring      m_workerDest;     // StartBackupAsync 用：目标路径
    std::string       m_workerCompress; // StartBackupAsync 用：压缩方式
    std::wstring      m_workerName;     // StartBackupAsync 用：子镜像名
    std::string       last_err_;        // worker 错误回传
    // 进度节流（100ms）
    std::chrono::steady_clock::time_point m_lastProgressPost;
    std::chrono::steady_clock::time_point m_start{};
    std::wstring m_lastStage;  // WM_TIMER 刷新「已用」时复用
    static constexpr int kProgressThrottleMs = 100;
};
