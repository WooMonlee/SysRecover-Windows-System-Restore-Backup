#pragma once
// 镜像"读取层"：把普通 WIM/ESD 与加密容器（.zje）统一成同一套只读查询 +
// "变成 wimlib 能打开的明文路径"。
//
// 为什么需要这一层：
//   · wimlib 只能按**路径**打开文件、且是随机访问，没法接自定义解密流；
//   · 加密容器里带一份**加密的子镜像清单**（crypt.h 的 meta），所以列表/空间
//     预估/winload 预检都不必先解密几十 GB；
//   · 只有真正要解压/校验时才解密到同卷临时文件（用完立即删）。
//
// 口令只放内存（GUI 输入框 / CLI --password），绝不写日志、契约、history。
#include <string>
#include <vector>

#include "common/crypt.h"
#include "wim/wim.h"

namespace sysrecover {

// 进程内"镜像口令"表。GUI 选中 .zje 时询问一次后写入，后续各层透明复用。
void SetImagePassword(const std::wstring& path, const std::wstring& pwd);
std::wstring GetImagePassword(const std::wstring& path);
void ClearImagePasswords();

// 是否加密容器（读前 8 字节 magic；不要求文件存在时返回 false）。
bool ImageIsEncrypted(const std::wstring& path);

// ── 只读查询（加密镜像走容器 meta，不解正文）──
int ImageProbe(const std::wstring& path, std::wstring& why);
int ImageList(const std::wstring& path, std::vector<ImageDesc>& out);
// 子镜像未压缩内容量（A 引擎空间预检用）。
int ImageContentSize(const std::wstring& path, int index,
                     unsigned long long* bytes);
// 是否含 \Windows\system32\winload.exe（不支持的系统预检）。
int ImageHasWinload(const std::wstring& path, int index, bool* exists);

// ── 明文解析（只有解压/校验/extract 才需要）──
// 非加密 → 原样返回（*temp=false）；
// 加密   → 用进程口令解密到同卷临时文件（*temp=true；调用方负责 Release）。
// 返回空串表示失败（err 为原因；进度回调可空）。
std::wstring ResolveImagePlain(const std::wstring& path, ProgressFn progress,
                               std::wstring& err, bool* temp);
void ReleaseImagePlain(const std::wstring& path);
void ReleaseAllImagePlain();

}  // namespace sysrecover
