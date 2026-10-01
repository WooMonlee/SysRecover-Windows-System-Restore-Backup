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

#include "common/sysinfo.h"  // SystemDescription（备份信息/默认文件名）
#include "app/advice.h"       // ErrAdvice（worker 建议码回传；头文件零依赖，可单独 include）

using namespace DuiLib;

// 自定义 WM_APP 消息：worker 线程 → 主线程 UI 更新
// ⚠️ 必须从 WM_APP+10 起，禁止用 WM_APP+1 —— Duilib 的 CPaintManagerUI 内部
// 用 WM_APP+1 做**异步通知泵**（PostAsyncNotify → PostMessage(WM_APP+1, 0, 0)，
// 消费点在 WindowImplBase::HandleMessage 更深处的 m_PaintManager.MessageHandler）。
// 曾与 WM_PROGRESS_UPDATE 撞车：我们先截获并把它的 lParam=0 当 wstring* 解引用
// → 空指针崩溃（combo 第二次重建触发延迟清理时实测，PIT-096）。
// 顺带影响：撞车前 Duilog 的异步通知也从未被泵过（标志位卡死）。
#define WM_PROGRESS_UPDATE (WM_APP + 10)  // wParam=pct, lParam=ptr to std::wstring
#define WM_TASK_COMPLETE   (WM_APP + 11)  // wParam=rc (0=成功)
// 跨进程查询：返回 1 = 正在执行备份/还原任务（拒绝被关闭），0 = 空闲。
// 由新启动的实例发给已有实例（main_win.cpp 的单实例流程），同权限下可通。
#define WM_SR_QUERY_BUSY   (WM_APP + 12)
// EN_SETFOCUS 后延迟弹「搜索结果」下拉（见 HandleMessage 同名分支，PIT-097）。
#define WM_OPEN_PICK_MENU  (WM_APP + 13)

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
    // 「搜索」按钮（还原模式）：扫附近目录的 .esd/.wim（imgsearch.cpp），把最新一个
    // 填入第一步输入框；结果留在 m_searchHits 供点输入框时下拉选择。
    void OnSearchImages();
    void ShowImagePickMenu();   // 输入框获得焦点 → TrackPopupMenu 列 m_searchHits
    void HandleDroppedFiles(WPARAM wParam);  // WM_DROPFILES：拖入的镜像填进第一步
    void LoadWimImages(const std::wstring& path);
    void StartRestore();   // UI 校验 + 确认 → StartRestoreAsync
    void StartBackup();    // UI 校验 + 确认 → StartBackupAsync
    void StartRestoreAsync();  // worker 线程：StageRestore
    void StartBackupAsync();   // worker 线程：RunBackup
    void ApplyModeUi();
    void UpdateMainAction();
    // 从界面（原生 EDIT 子窗口）回读镜像路径 → 同步 m_wimPath、必要时解析镜像、刷新按钮。
    // 供 EN_CHANGE 与 500ms 兜底定时器共用（用户 2026-09-23：手输/粘贴后按钮不亮）。
    void SyncImagePathFromUi();
    // 备份模式：按第一步的文件后缀挑「格式」下拉（.wim → .wim(正常大小)）。
    void SyncFormatFromPath();
    // ★ 2026-09-30 用户规格：换「类型」→ 同步改文件名后缀（只改后缀，其余不动）。
    void SyncPathExtFromFormat();
    // 「限制CPU」下拉 → m_cpuCap；备份中改了会立即作用到正在跑的任务。
    void ApplyCpuCapFromUi();
    void ToggleBootMenu();       // 「安装/删除启动还原」按钮
    void RefreshBootMenuBtn();   // 按是否已安装刷新按钮文字
    void RebootNow();
    bool AskBusyClose();          // 忙时关闭：true = 用户选了「终止并退出」
    // BitLocker 提醒（有加密卷时提醒"没密钥则数据无法恢复"）：true = 继续
    bool AskBitLockerWarning(const std::vector<std::wstring>& vols);
    // 目标盘健康警告（PIT-086）：坏盘 → 弹框（默认**取消**=安全项）；取不到 SMART 直接放行。
    bool AskDiskHealthWarning(int diskIndex);
    void CancelAndExit();         // 中止 worker → 清理未完成镜像 → 关窗
    void CleanupIncompleteOutput();
    bool IsSilent();   // 「静默模式」勾选态
    bool IsEspChecked();   // 「ESP」勾选态（备份专属，2026-09-30：ESP 并入主镜像）
    void SetStatus(const std::wstring& text);
    void SetProgress(int pct);
    // 进度条（+百分比）只在任务执行中显示；静止时整行状态栏让给状态文字（见 .cpp）。
    void ShowProgress(bool show);
    void OnProgressUpdate(int pct, const std::wstring& stage);
    void OnTaskComplete(int rc);
    int  BackupFmt() const;  // combo 0/1/2
    std::wstring ElapsedText() const;  // 已用 mm:ss / h:mm:ss

    bool m_backupMode = false;
    bool m_needReboot = true;  // 还原：true=已暂存需重启；false=已就地完成（PIT-064）
    bool m_imageOk = false;    // 还原模式：当前镜像可读/可用（否则主按钮保持灰）
    std::wstring m_wimPath;
    std::wstring m_lastLoadedWim;  // 上次已解析过子镜像的路径（避免手动输入时反复解析）
    sysrecover::SystemDescription m_sysDesc;  // 运行中系统的描述（InitWindow 里取）
    int m_selPart = -1;
    int m_selImageIndex = 1;
    // 「限制CPU」下拉当前值（0=不限，25/50/75=整机 CPU 百分比上限，见 cpucap.h）
    int m_cpuCap = 0;
    std::vector<int> m_imgIdx;
    std::vector<sysrecover::PartitionInfo> m_parts;
    std::vector<std::wstring> m_searchHits;  // 最近一次「搜索」结果（修改时间新→旧）
    bool m_pickGuard = false;   // 下拉选择菜单重入防护（EN_SETFOCUS → TrackPopupMenu）

    // worker 线程
    std::thread       m_worker;
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_busy{false};
    std::atomic<bool> m_cancelling{false};  // 「终止并退出」流程已启动（防重入/重复弹框）
    int               m_lastRc = 0;
    std::wstring      m_workerSource;   // StartBackupAsync 用：源路径
    std::wstring      m_workerDest;     // StartBackupAsync 用：目标路径
    std::string       m_workerCompress; // StartBackupAsync 用：压缩方式
    std::wstring      m_workerName;     // StartBackupAsync 用：子镜像名
    int               m_workerCpuCap = 0;  // StartBackupAsync 用：CPU 上限（起线程前定格）
    bool              m_workerEsp = false; // StartBackupAsync 用：是否把 ESP 并入主镜像
    std::string       last_err_;        // worker 错误回传
    sysrecover::ErrAdvice last_adv_ = sysrecover::ADV_NONE;  // worker 建议码回传
    // 进度节流（100ms）
    std::chrono::steady_clock::time_point m_lastProgressPost;
    // 最近一次**真正发出**的阶段原串（worker 线程写）：阶段名一变就放行、不再
    // 被 100ms 节流吞掉 —— 否则 100% 后 100ms 内的收官标记（verify/probe）会丢，
    // 状态栏冻在上一阶段（PIT-098）。
    std::string m_lastStageRaw;
    std::chrono::steady_clock::time_point m_start{};
    std::wstring m_lastStage;  // WM_TIMER 刷新「已用」时复用
    std::wstring m_lastStatus; // 最近一条状态文字（进度条显隐后要按新宽度重新收放）
    static constexpr int kProgressThrottleMs = 100;
    HWND m_tipHwnd = NULL;   // 自管 tooltip（Duilib TTM_ADDTOOL 失败，见 PIT-089）
    TOOLINFO m_tipInfo{};
    std::wstring m_tipText;  // 当前 tooltip 文本（lpszText 要求存活到 SETTOOLINFO 之后）
};
