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
#include <exception>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "StdAfx.h"

#include "common/sysinfo.h"  // SystemDescription（备份信息/默认文件名）
#include "app/advice.h"       // ErrAdvice（worker 建议码回传；头文件零依赖，可单独 include）

// 只按引用传（不 include app/ops.h，避免把 std 头带进 Duilib 之前的顺序问题）；
// 完整定义在主窗体 .cpp 里可见。
namespace sysrecover {
struct RestoreRequest;
}

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
// 「日志」按钮：支持包导出完成（worker → 主线程，lParam = SupportBundle*）。
#define WM_SUPPORT_DONE    (WM_APP + 14)
// 启动时检查救援层失败回执（黑匣子）：见 ShowRescueFailureNotice。
#define WM_CHECK_RESCUE    (WM_APP + 15)
// 启动后置工作（问题 2，用户 2026-10-12 规格："USB 盘插入时启动 20 秒才出
// 界面，要把检查挪到界面显示之后"）：InitWindow 只建窗，这几件事 PostMessage
// 过来后**在后台线程**做，做完再 PostMessage 结果回主线程。
//   WM_STARTUP_LAZY：起后台线程（diag.txt/list.txt + 部署痕迹收集 + 散件清理
//                    + 磁盘枚举）——这三样都可能被慢速 USB2 设备卡住。
//   WM_PARTS_READY ：lParam = new std::vector<DiskInfo>，线程枚举完成 → 主线程
//                    用它填分区下拉（handler 里 delete 接管所有权）。
#define WM_STARTUP_LAZY    (WM_APP + 16)
#define WM_PARTS_READY     (WM_APP + 17)
// P3（PIT-148）「一键重试」worker 完成：wParam=rc, lParam=needReboot(0/1)；
// 失败原因在 m_retryErr（worker 线程写、主线程读，PostMessage 前已落成员）。
#define WM_RETRY_DONE      (WM_APP + 18)
// 「安装/删除菜单」worker 完成（2026-10-18）：原来装/删菜单在 UI 线程**同步**跑
// ~5 秒（写 28MB 救援载荷 + 双读 CRC 校验），Windows 判定"无响应"（幽灵窗）；
// 现在改为 worker + 进度：wParam=rc, lParam=install(0/1)；失败原因在 m_menuErr。
#define WM_MENU_DONE       (WM_APP + 19)// 新硬盘分区向导（2026-10-20）：worker 完成，wParam=rc；错误在 m_initErr。
#define WM_PART_INIT_DONE  (WM_APP + 23)
// 「修复引导」worker 完成：wParam=rc；说明在 m_fixMsg。
#define WM_FIXBOOT_DONE    (WM_APP + 24)
// 「镜像检查」（完整校验，2026-10-27）worker 完成：wParam=rc(0=通过)；
// 说明在 m_verifyMsg。放在「更多功能>>」菜单里（原「异机」勾选框位置）。
#define WM_VERIFY_DONE     (WM_APP + 25)
// 「安装到启动」的能力扫描（2026-10-28 用户规格：扫描/解压/取消 三选）：
// worker 完成，wParam=1 表示"可在救援层直接还原"；明细在 m_scanBad/m_scanEnc。
#define WM_MENU_SCAN_DONE  (WM_APP + 26)

namespace sysrecover {
struct PartitionInfo;
struct SupportBundle;
struct DiskInfo;
enum class PartitionStyle;
}

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
    // pre-message filter 覆写（WindowImplBase::OnCreate 已把 this 注册为
    // pre-message filter，WinImplBase.cpp:275，虚分派先到我们）：拦 Ctrl+Tab
    // 切模式 —— 裸 Tab 在 PreMessageHandler 的 switch 里就被 SetNextTabControl
    // 吃掉（UIManager.cpp:671-676），HandleMessage 永远收不到，只能在这层拦。
    virtual LRESULT MessageHandler(UINT uMsg, WPARAM wParam, LPARAM lParam,
                                   bool& bHandled) override;
    // 自绘控件注入点（未知 UIClass 回落到这里，见 ui_skin.h 头部注释）
    virtual CControlUI* CreateControl(LPCTSTR pstrClass) override;

private:
    // 分区下拉填充：磁盘表由调用方传入（启动时来自后台线程的 WM_PARTS_READY，
    // 省掉在 UI 线程上重复 EnumerateDisks —— 问题 2，2026-10-12）。
    void PopulatePartitions(const std::vector<sysrecover::DiskInfo>& disks);
    // 启动后置工作（后台线程体，见 WM_STARTUP_LAZY）：diag/痕迹收集/清理/枚举。
    void StartupLazyWork();
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
    // 还原引擎选择（2026-10-14 用户规格：确认框三选）：
    //   "swap"  = 还原后重启（A 引擎，Windows 解压 + Linux 交换，可返回旧系统）
    //   "block" = 重启后还原（B 引擎，Linux 直写，C 盘内容全丢）
    // 由 StartRestore 在确认框里选定，StartRestoreAsync 消费。
    std::string m_restoreEngine = "swap";
    bool        m_restoreKeepOld = false;  // swap 时 true=保留 ~old 可返回；block 恒 false
    void ApplyModeUi();
    void UpdateMainAction();
    std::wstring& ActivePath() {
        return m_backupMode ? m_backupPath : m_wimPath;
    }
    void SetImagePathEdit(const std::wstring& p);
    void SetBusyControls(bool busy);
    // 从界面（原生 EDIT 子窗口）回读镜像路径 → 同步 m_wimPath、必要时解析镜像、刷新按钮。
    // 供 EN_CHANGE 与 500ms 兜底定时器共用（用户 2026-09-23：手输/粘贴后按钮不亮）。
    void SyncImagePathFromUi();
    // 备份模式：按第一步的文件后缀挑「格式」下拉（.wim → .wim(正常大小)）。
    void SyncFormatFromPath();
    // ★ 2026-09-30 用户规格：换「类型」→ 同步改文件名后缀（只改后缀，其余不动）。
    void SyncPathExtFromFormat();
    // 「限制CPU」下拉 → m_cpuCap；备份中改了会立即作用到正在跑的任务。
    void ApplyCpuCapFromUi();
    void ToggleBootMenu();       // 「安装到启动」按钮（UI 线程：确认 → StartMenuToggle）
    // 「更多功能>>」下拉（2026-10-27 用户规格）：原「异机」勾选框、「修复引导」、
    // 原「清除菜单」、新增「镜像检查」四个入口收进这里。用原生 TrackPopupMenu
    // （与 ShowImagePickMenu 同款），不改皮肤结构。
    void ShowMoreMenu();
    // 清除启动中还原菜单（原 RepairBootBtn 的逻辑）：确认 → 异步删引导层 + 绑定契约。
    void ClearBootMenuEntry();
    // 镜像检查（完整校验）：worker 里跑 WimEngine::Verify，完成走 WM_VERIFY_DONE。
    void StartVerifyImage();
    void VerifyImageAsync();
    void StartMenuToggle(bool install,
                         const std::string& engine = "swap");  // 异步入口
    // 「安装到启动」→「扫描」（2026-10-28 用户规格）：先只读检查镜像能否
    // **只靠救援层**还原（block 引擎），能就免解压安装，不能就提示改用「解压」。
    void StartMenuScan();
    void OnMenuScanDone(bool linuxOnly);
    void MenuToggleAsync(bool install,
                         const sysrecover::RestoreRequest& req);  // worker 线程体
    void OnMenuDone(int rc, bool install);  // worker 完成回主线程（WM_MENU_DONE）
    void RefreshBootMenuBtn();   // 按是否已安装刷新按钮文字
    // 「日志」按钮（2026-10-03 用户规格）：一键收集 各盘日志 + explorer 转储 +
    // 事件日志 → 打包到桌面；结果框里顺便提供「进入/退出安全模式」。
    void ExportSupportFlow();
    void OnSupportDone(sysrecover::SupportBundle* r);
    // 启动时（收集完 diag/list/痕迹后）检查救援层失败回执（用户 2026-10-04
    // 规格 2）：有未报告的失败 → 弹 3 行提示，「打开日志文件夹」直达 collected。
    void ShowRescueFailureNotice();  // P3（PIT-148）：A/B 类失败 → Ask3 + 一键重试
    void StartRetryStagedRestore();  // 一键重试 worker 入口（读 conf → StageRestore）
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
    // 异机驱动注入阶段：主进度已 100%，用独立小进度条做"仍在工作"提示。
    void ShowPostProgress(bool show);
    void OnProgressUpdate(int pct, const std::wstring& stage);
    void OnTaskComplete(int rc);
    // 2026-10-20：新硬盘分区向导（第二步分区下拉为空时直接打开）与「修复引导」。
    void OpenInitDiskWizard();
    // 修复引导（2026-10-28）：confirm=false 用于「被安全软件拦截 → 重试」的二次进入
    // （重试时不再重复确认框，直接开跑）。
    void StartRepairBoot(bool confirm = true);
    // 异机还原（2026-10-21，0.9）：勾选框状态。
    bool IsUniversalChecked();
    int  BackupFmt() const;  // combo 0/1/2
    std::wstring ElapsedText() const;  // 已用 mm:ss / h:mm:ss

    bool m_backupMode = false;
    bool m_needReboot = true;  // 还原：true=已暂存需重启；false=已就地完成（PIT-064）
    bool m_imageOk = false;    // 还原模式：当前镜像可读/可用（否则主按钮保持灰）
    std::wstring m_wimPath;       // 还原镜像
    std::wstring m_backupPath;    // 备份保存位置（与还原路径分离）
    std::wstring m_lastLoadedWim;  // 上次已解析过子镜像的路径（避免手动输入时反复解析）
    sysrecover::SystemDescription m_sysDesc;  // 运行中系统的描述（InitWindow 里取）
    int m_selPart = -1;
    int m_selImageIndex = 1;
    // 「限制CPU」下拉当前值（0=不限，25/50/75=整机 CPU 百分比上限，见 cpucap.h）
    int m_cpuCap = 0;
    std::vector<int> m_imgIdx;
    std::vector<sysrecover::PartitionInfo> m_parts;
    // 与 m_parts 平行：每个候选分区所在磁盘的分区表风格（GPT/MBR）。
    // 2026-10-20：第三步确认框按"当前启动方式 + 目标风格"给提示（只提示不阻止）。
    std::vector<sysrecover::PartitionStyle> m_partStyles;
    // 新硬盘分区向导 / 修复引导 的 worker 回传（主线程读）。
    std::wstring m_initErr;
    std::string m_initLog;
    std::string m_fixMsg;
    // 2026-10-28：本次「修复引导」/「安装到启动」的失败是否由安全软件拦写引起
    // （worker 线程里 TakeBootBlocked() 取走，done 消息回主线程后据此弹「重试」）。
    bool m_fixBlocked = false;
    bool m_menuBlocked = false;
    std::string m_verifyMsg;   // 镜像检查结果说明（worker 写、主线程读）
    std::vector<std::wstring> m_searchHits;  // 最近一次「搜索」结果（修改时间新→旧）
    bool m_pickGuard = false;   // 下拉选择菜单重入防护（EN_SETFOCUS → TrackPopupMenu）

    // worker 线程
    std::thread       m_worker;
    std::thread       m_startWorker;  // 启动后置工作线程（问题 2；析构里 join）
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_busy{false};
    std::atomic<bool> m_cancelling{false};  // 「终止并退出」流程已启动（防重入/重复弹框）
    int               m_lastRc = 0;
    std::wstring      m_workerSource;   // StartBackupAsync 用：源路径
    std::wstring      m_workerDest;     // StartBackupAsync 用：目标路径
    std::string       m_workerCompress; // StartBackupAsync 用：压缩方式
    std::wstring      m_workerName;     // StartBackupAsync 用：子镜像名
    int               m_workerCpuCap = 0;  // StartBackupAsync 用：CPU 上限（起线程前定格）
    bool              m_workerEsp = false;
    std::vector<std::wstring> m_skippedFiles; // StartBackupAsync 用：是否把 ESP 并入主镜像
    std::string       last_err_;        // worker 错误回传
    sysrecover::ErrAdvice last_adv_ = sysrecover::ADV_NONE;  // worker 建议码回传
    // P3（PIT-148）「一键重试」worker 错误回传（PostMessage 前已落成员，
    // 主线程 WM_RETRY_DONE 读 —— 不传指针，避免生命周期问题）。
    std::string       m_retryErr;
    // 装/删菜单 worker 回传（2026-10-18，同上：成员回传、不传指针）。
    std::string       m_menuErr;
    // 「安装到启动」扫描结果（2026-10-28）：能否只靠救援层还原 + 原因计数。
    size_t            m_scanBad = 0;
    size_t            m_scanEnc = 0;
    bool              m_menuInstall = false;  // 本次操作是装(true)还是删(false)
    // 进度节流（100ms）
    std::chrono::steady_clock::time_point m_lastProgressPost;
    // 最近一次**真正发出**的阶段原串（worker 线程写）：阶段名一变就放行、不再
    // 被 100ms 节流吞掉 —— 否则 100% 后 100ms 内的收官标记（verify/probe）会丢，
    // 状态栏冻在上一阶段（PIT-098）。
    std::string m_lastStageRaw;
    std::chrono::steady_clock::time_point m_start{};
    std::wstring m_lastStage;  // WM_TIMER 刷新「已用」时复用
    std::wstring m_lastStatus; // 最近一条状态文字（进度条显隐后要按新宽度重新收放）
    bool m_postBusy = false;   // 异机驱动注入阶段的小进度条正在跑
    int  m_postPulse = 0;      // 小进度条的循环值（0..100）
    static constexpr int kProgressThrottleMs = 100;
    HWND m_tipHwnd = NULL;   // 自管 tooltip（Duilib TTM_ADDTOOL 失败，见 PIT-089）
    TOOLINFO m_tipInfo{};
    std::wstring m_tipText;  // 当前 tooltip 文本（lpszText 要求存活到 SETTOOLINFO 之后）
};
