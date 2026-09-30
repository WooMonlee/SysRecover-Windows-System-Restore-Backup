#pragma once
// 镜像「搜索」（还原模式第一步的「搜索」按钮）—— 扫描附近目录的 .esd/.wim。
//
// 扫描规则（用户 2026-09-30 规格，按序判定，命中即止）：
//   1. 单EXE：exe 位于 %TEMP% 且取得到父进程 exe 路径
//      → 扫「父进程 exe 所在目录」+「其上一级」（各仅一层）
//      （单EXE 被打包器解压到 %TEMP% 运行，真实语境在父进程那边）
//   2. exe 装在 Program Files / Program Files (x86) 体系下
//      → 遍历所有非系统分区：盘根一层 + 每个一级子目录内一层（不再往下）
//   3. 正常 → 扫「exe 所在目录的上一级」（仅该层）
// 除规则 2 外均不递归子目录。返回完整路径，按文件修改时间新→旧排列。
#include <string>
#include <vector>

namespace imgsearch {

std::vector<std::wstring> Search();

}  // namespace imgsearch
