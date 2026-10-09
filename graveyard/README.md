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
| `tests/`（单元测试、崩溃探针、版本测试） | 用户裁定：换架构后测试全部重做，旧测试无意义 | 新软件测试矩阵（docs/21 · P3） |
| `tools/ea-scan.cpp` | EA 时代取证小工具 | Windows 原生提取后不再需要 |
| `tools/vmtest/` 的 23 个测试脚本（smoke/drill/布局/探针） | 旧架构的 QEMU 演练与冒烟 | 新软件回归矩阵（docs/21 · P3） |

> 保留在 `tools/vmtest/` 的：`build-alpine-initramfs.py`（救援构建输入）、`dl/`（apk 缓存）、
> `mygzip.py`、`parse-initramfs.py`、`elf-deps.py`、`README.md` —— 这些是**开发/构建工具**，不是旧测试。

## 待移入（随新软件开发推进；先在 docs/21 登记）
- **EA/reppack 链**（`src/common/ea.*`、`src/tools/ea_apply_main.cpp`、`bootfiles/zj-ea-apply.exe`）——
  目前仍是"悬空引用清理"的投放载体，待新架构的投放方式定稿后移入；
- **`bootfiles/zjrestore-lite.sh`**（apply 型救援脚本）——待新救援（改名交换引擎）写好后移入；
- 其余按 `docs/21` 的"淘汰清单"逐项处理。
