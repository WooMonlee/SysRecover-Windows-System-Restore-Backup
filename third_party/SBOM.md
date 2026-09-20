# 第三方组件清单（SBOM，随版本更新）

> 规则见 AGENTS.md §2/§14：只允许动态链接 LGPL 库 + 分发 GPL 二进制（不改不链接）；
> 禁止引入 `wimlib-imagex` 源码、grub4dos 源码、Dism++ 代码。

| 组件 | 版本/来源 | License | 本地位置 | SHA256 | 链接/分发方式 |
|---|---|---|---|---|---|
| Duilib（经典版，**在用**） | 2026-09-07 master zip（`duilib/duilib` 官方仓库） | MIT/BSD 混合（源文件头各自声明） | `third_party/duilib-master/`（源码）+ 同目录 zip 留底 | `9650BB59A1EDC7DD61A452C60078D0F15D8FF5A697384A10CD095854DCFD64D4`（zip） | 35 cpp 静态编入 `build/libduilib.a` → GUI；MinGW 补丁见 PIT-012 |
| nim_duilib（**已弃用**） | 2026-09-05 main 分支 zip（`rhett-lee/nim_duilib`） | MIT | `third_party/nim_duilib-main/`（源码）+ 同目录 zip 留底 | `B2246847092C09831AF07C6CC761727469EDB81915018D3E67FB4C72E42DDA18`（zip） | 弃用原因：运行时强制 Skia（无 GDI 回退），体积违背 <10MB 门禁；仅留档不编译 |
| libwim | wimlib 官方 1.14.5 包（旧项目 `tools\ref\` + dist） | LGPLv3 | `third_party/wimlib/`（`wimlib.h` + `libwim-15.dll`） | DLL `BA853EE1…508495`（全值见下） | MinGW 直连 DLL 动态链接；保留声明，允许用户替换 |
| grldr / grldr.mbr | GRUB4DOS 0.4.6a（`chenall/grub4dos` release `2020-08-09-0da21fe`） | GPL | `bootfiles/` | grldr `DECE3F8D…39128D` / grldr.mbr `F5C6E8E2…9ACEF` | 仅分发二进制，不修改不链接，独立聚合 |
| vmlinuz / initramfs / restore.sh | 自有资产（旧项目沿用） | — | `bootfiles/` | vmlinuz `7E55ECE3…92025` / initramfs `2EDF9225…A67483` | 只读引用 |

完整哈希：
- `duilib-master.zip`: `9650BB59A1EDC7DD61A452C60078D0F15D8FF5A697384A10CD095854DCFD64D4`
- `nim_duilib-main.zip`: `B2246847092C09831AF07C6CC761727469EDB81915018D3E67FB4C72E42DDA18`
- `libwim-15.dll`: `BA853EE1E3FC5F5798581F02E8E066BA07A0A2375F0BF444FE981431FD508495`
- `grldr`: `DECE3F8D20F84AE0D0FB892B5C3A2D19E7233D0D8885B0027A6F43D77239128D`
- `grldr.mbr`: `F5C6E8E2C1EB7380285FA9CB1C9168E92D5B3B55CDE052C043BA81ED17B9ACEF`

MinGW 兼容验证：
- nim_duilib（2026-09-05，GCC 14.2.0）：`duilib/duilib.h` `-fsyntax-only` 通过；`duilib/Core/` 61 个 `.cpp` 编译通过（`-std=c++20 -O1`）；链接阶段发现运行时强制 Skia，弃用。
- 经典 Duilib（2026-09-07，GCC 14.2.0）：35/35 cpp + `stb_image.c` 编译通过（`-std=c++17 -O1 -fpermissive -D_stdcall=__stdcall`，补丁见 PIT-012）；`libduilib.a` 1.2MB；`SysRecoverUI.exe` 1.84MB 链接成功，870×410 窗口显示 + 分区枚举验证通过。
