#pragma once
#include <string>

// 备份前置检查：卷影复制服务（VSS）（用户 2026-09-26 规格）
// 热备依赖 wimlib 的 WIMLIB_ADD_FLAG_SNAPSHOT → Windows VSS 卷影快照；服务被禁用/
// 缺失时 wimlib 只报 rc=89 "Unable to create a filesystem snapshot"（只字不提服务名），
// 用户完全无从下手。这里提前查：
//   · 能启动就启动（记录原状态）；
//   · 起不来（禁用/缺失/超时/WoW64）→ err = 多行中文处理指引，阻断在动数据之前；
//   · 备份结束（成功/失败）把我们启动的服务停回原状（"完成后关闭"）。
// 实测（2026-09-27，Windows 11 dev 机）：
//   · vss=禁用 或 swprv=禁用 → 备份均 rc=89 失败（两者都是硬依赖，一个都不能少）；
//   · 两服务「停止+手动」（Windows 默认态）→ wimlib 自己能拉起，但**跑完不关**
//     （留 Running 残留，与"完成后关闭"的规格不符，所以由我们自己管生命周期）；
//   · 32 位程序跑在 64 位系统上 → wimlib.h 明文 VSS 快照不支持 WOW64，必失败。
namespace vss {

class BackupGuard {
public:
    ~BackupGuard();  // 我们启动的服务 → 停回原状（覆盖 RunBackup 的所有退出路径）

    // 在 snapshot=true 时调用。成功 true（可继续备份）；
    // 不可用 → err=处理指引（\r\n 多行；GUI 走 MessageBox、CLI 直接打印），返回 false。
    // 幂等：没调用过 / 失败退出时，析构只动「亲眼确认过当时是停止、且现在在跑」的服务。
    bool Ensure(std::wstring& err);

private:
    // 默认 true = "不了解状态就不碰"：只有亲眼确认过「备份前是停止」的才允许停回。
    bool vss_was_running_ = true;
    bool swprv_was_running_ = true;
};

}  // namespace vss
