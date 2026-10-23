# graveyard —— 代码坟场（2026-10-09 起）

> 目的：为「新软件」（Windows 原生解压 + Linux 仅做目录改名交换，见 `docs/21`）
> 腾出干净工作区。**判定"用不到"的内容一律移到这里，不直接删除**——日后若需要，
> 可在此或 git 历史里找回。**只进不出**（清空须用户裁定）。

## 规则
- 移入前：先从构建/文档引用中摘除（Makefile、check-docs 等），保证 `make check` / `make package` 仍通过；
- 移入后：在下表记一行（来源 / 原因 / 替代物）。

## 已移入（批次 1，2026-10-09）
| 内容 | 原因 | 替代 |
|---|---|---|
| ~~`tests/`（单元测试、崩溃探针、版本测试）~~ **已迁回 `tests/`（2026-10-11）** | 曾裁定换架构后重做；重做时旧 45 用例对 0.7 源码复验全绿、原样恢复，并补 0.7 决策表用例 | 现役：`make check`（单测+门禁）/ `make crash-test` |
| `tools/ea-scan.cpp` | EA 时代取证小工具 | Windows 原生提取后不再需要 |
| `tools/vmtest/` 的 23 个测试脚本（smoke/drill/布局/探针） | 旧架构的 QEMU 演练与冒烟 | 新软件回归矩阵（docs/21 · P3） |

> 保留在 `tools/vmtest/` 的：`build-alpine-initramfs.py`（救援构建输入）、`dl/`（apk 缓存）、
> `mygzip.py`、`parse-initramfs.py`、`elf-deps.py`、`README.md` —— 这些是**开发/构建工具**，不是旧测试。

## 待移入（随新软件开发推进；先在 docs/21 登记）
- **EA/reppack 链**（`src/common/ea.*`、`src/tools/ea_apply_main.cpp`、`bootfiles/zj-ea-apply.exe`）——
  目前仍是"悬空引用清理"的投放载体，待新架构的投放方式定稿后移入；
- **`bootfiles/zjrestore-lite.sh`**（apply 型救援脚本）——待新救援（改名交换引擎）写好后移入；
- 其余按 `docs/21` 的"淘汰清单"逐项处理。
