# PIT 坑位册（全量正文）

> 本文件是 [`AGENTS.md`](../AGENTS.md) §13 的**全量正文**（2026-10-28 迁出；约 366 KB）。
> `AGENTS.md §13` 只保留**活跃约束索引**；本文件**不随会话注入上下文**，按需检索。

## 0. 引用约定（改编号 = 全仓 600+ 处注释失联）

- 全仓代码/文档注释**只写编号**（如 `// PIT-135`）→ **编号即锚点**：`PIT-135` 对应本文件 `#pit-135`。
- 编号**只增不改、不重排、不复用**；新坑**追加到本文件末尾**（取下一个空号），并在 `AGENTS.md §13` 决定是否登记为"活跃约束"。
- **`PIT-066` 有两条**（前者"走法对比"、后者"零注册正解（已实测通过）"）：代码/文档引用 `PIT-066` 时指的是**后者**（Canonical/Debian 签名链）。
- **`PIT-136` 原为空号**：正文补在下面（内容实际并入 `PIT-135`/`PIT-157`），用于消除 `docs/20` 与 8 处代码注释的悬空引用。
- **历史作废**：`PIT-022~043`（BG-Rescue 底座）、`PIT-052`（被 053 推翻）、`PIT-061`、`PIT-063`、`PIT-065`、`PIT-066`（一）—— 保留作历史，勿按它们施工。

## 1. 坑位正文（按编号顺序）

### PIT-001

- PIT-001 BCD `path` 写成 `\grldr`：必须 `\grldr.mbr`（grldr.mbr 才是 bootmgr 可加载的启动扇区）。✅ 2026-09-05

### PIT-002

- PIT-002 漏 `/application bootsector`：会被当 osloader，加载 Linux 内核必失败。✅ 2026-09-05

### PIT-003

- PIT-003 全文匹配 GUID 误判：`{bootmgr}` 的 `bootsequence` 值里也有该 GUID。正确：`bcdedit /enum {GUID}` 看输出是否回显 GUID。✅ 2026-09-05

### PIT-004

- PIT-004 PowerShell 花括号：PS 里标识符必须加引号，C++ `CreateProcessW` 无需处理。✅ 2026-09-05

### PIT-005

- PIT-005 .NET 8 GBK 乱码（C# 遗留，C++ 警示）：`Encoding.GetEncoding(936)` 需注册 CodePages provider，否则中文变 `?`。C++ 侧对策：存在性判定只用 ASCII GUID，不解析中文。✅ 2026-09-05

### PIT-006

- PIT-006 menu.lst 语法：GRUB4DOS 用 `kernel`+`initrd`，不是 GRUB2 的 `linux`。✅ 2026-09-05

### PIT-007

- PIT-007 MinGW `wprintf` 的 `%s` 是窄字符串：MinGW 链接 msvcrt.dll，其 `wprintf` 把 `%s` 当 `char*`（读到宽字符的 0x00 即停，输出为空）。宽字符串**必须用 `%ls`**。✅ 2026-09-05

### PIT-008

- PIT-008 `WIMLIB_*_FLAG_NTFS` 是**裸卷模式**，不是"启用 NTFS 语义"：`ADD_FLAG_NTFS` 要求源为**未挂载 NTFS 卷**，`EXTRACT_FLAG_NTFS` 要求目标为裸卷（且需 libntfs-3g，否则 `WIMLIB_ERR_UNSUPPORTED=68`）。**目录捕获/应用一律 flags=0**（NTFS 的 ACL/流在普通模式下本来就会处理；热备加 `WIMLIB_ADD_FLAG_SNAPSHOT` 即可）。✅ 2026-09-05

### PIT-009

- PIT-009 热备必须传排除配置：`config=NULL` 时易失文件（pagefile/日志/Temp/云同步目录）在扫描与读取之间被改写 → `rc=88 WIMLIB_ERR_CONCURRENT_MODIFICATION_DETECTED`。做法：`src/wim/exclude.cpp` 生成默认清单 + 源内云目录动态检测（对齐旧 `ExclusionService`），`backup` 必传。✅ 2026-09-05

### PIT-010

- PIT-010 `CreateProcessW` 裸文件名找不到系统工具：`L"bcdedit.exe"` 直接传会 `fail 2`（搜索顺序坑），必须 `GetSystemDirectoryW` 拼全路径（见 `SysToolPath()`；`manage-bde.exe` 同理）。✅ 2026-09-05

### PIT-011

- PIT-011 nim_duilib MinGW 直编三要素（已验证 329/329）：①排除 `CEFControl/RenderSkia/WebView2` 目录 + `**/{contrib,scripts,examples,test}` + `duilib.cpp`（纯 CEF 引用）+ `Image_LOTTIE.cpp`/`ImageDecoder_SVG.cpp`（需 Skia）；②加 `-D_stdcall=__stdcall`（RichEdit 头用的 MSVC 单下划线写法，**不改第三方源码**）；③加 `-I duilib/third_party/libwebp`（libwebp 内部头用相对路径）。✅ 2026-09-06

### PIT-012

- PIT-012 经典 Duilib MinGW 直编七要点（已验证 35/35 + GUI 链接）：①`StdAfx.h` 的 min/max 函数宏移到所有系统头之后，且定义前先 `#undef min/max`（libstdc++ `c++config.h` 会 undef，`minwindef.h` 会 define，状态不确定）；②**任何含 Duilib 头的编译单元，全部 C++ 标准头（尤其 `<functional>/<algorithm>/<thread>/<chrono>`）必须先于 `StdAfx.h` include**，否则 `bits/algorithmfwd.h` 的 `std::min/max` 模板与宏冲突（症状：`macro "max" passed 3 arguments` 或 StdAfx.h `#define max(a,b)` 行报 `expected unqualified-id before '(' token`——报错行是宏定义处，实为后续使用处冲突）；③加 `-fpermissive`（CDelegateStatic 函数指针→void* 等历史写法）；④`__super` 为 MSVC 专有 → 显式基类名（UICombo.cpp）；⑤`goto err` 跨初始化作用域 GCC 硬错误 → `return FALSE` 语义等价（UIRichEdit.cpp 4 处）；⑥`return false` 到指针类型（HFONT/桶数组）→ `return NULL`（UIManager.cpp/Utils.cpp）；GDI+ `DrawImage` 重载歧义 → 显式 `(INT)` 强转（UIGifAnim.cpp）；⑦排除 `UIFlash.cpp/UIWebBrowser.cpp`（需 ATL）并注释 `UIDlgBuilder.cpp` 的 `CWebBrowserUI` 分支；`Utils/stb_image.c` 必须加入构建（UIRender.cpp 引用 `stbi_load_from_memory`）。✅ 2026-09-07

### PIT-013

- PIT-013 QEMU 下引导本项目内核**必须 `-cpu max`**：`vmlinuz-zjrestore`（5.15.12）使用了 qemu64 默认 CPU 不具备的指令集，启动即静默三重故障 → 重启循环，VGA/串口**零输出**（症状极易误判为"内核/镜像损坏"）。判活方法：`-nographic` 日志中 "Booting from Hard Disk" 出现 ≥2 次 = 崩溃循环；1 次且持续运行 = 存活。✅ 2026-09-09（QEMU 11.1.1 TCG）

### PIT-014

- PIT-014 该 rescue 内核**无任何控制台输出**：`console=ttyS0` / `console=tty0` / `earlyprintk` 全部静默（内核疑似裁剪了 8250/VT 控制台驱动）。QEMU 里观察 Linux 阶段只能靠侧信道：① GRUB 阶段 `-nographic` VGA 镜像日志；② 磁盘写副作用（restore.sh 的持久化日志，`mdir` 查虚拟盘）；③ CPU 占用行为。**重建内核时务必开 `CONFIG_SERIAL_8250_CONSOLE=y`**（对齐 PLAN.md §4"进度供串口/日志采集"）。✅ 2026-09-09

### PIT-015

- PIT-015 Duilib 的 `caption` 属性第 3/4 个值是**距右/下边缘的偏移量**，不是右/下坐标：写成 `caption="0,0,870,36"`（照搬窗口宽度 870）会让 `WindowImplBase::OnNcHitTest` 的命中条件 `pt.x < rcClient.right - rcCaption.right` 退化成 `pt.x < 0` 恒不成立 → 永不返回 `HTCAPTION`，**整个窗口拖不动**（症状：控件能点、界面能正常交互，就是没法拖动，且无任何报错，极易误判为"Duilib 不支持无边框拖动"）。正确写法 `caption="0,0,0,36"`。验收：`SendMessage(hwnd, WM_NCHITTEST, 0, MAKELPARAM(screenX, screenY))` —— 顶栏空白/标题处应返回 `2 HTCAPTION`，标题栏上的最小化/最大化/关闭按钮与模式 Tab 应返回 `1 HTCLIENT`（保证仍可点击）。成因：`skin/main.xml` 由设计稿转正式皮肤时把 `0,0,0,36` 误抄成 `0,0,870,36`（`resources/themes/default/main/main.xml` 里一直是对的）。✅ 2026-09-13

### PIT-016

- PIT-016 经典 Duilib 皮肤七坑（把老 WPF 界面 1:1 搬到 Duilib 时实测，落地代码 `src/gui/ui_skin.cpp` + `skin/main.xml`）：①控件级圆角**只有** `borderround="cx,cy"`，且 `cx` 是**椭圆直径**（要 7px 圆角写 `14,14`）—— `roundcorner` **只认 `<Window>`**，写在控件上被静默忽略；②`pos="l,t,r,b"` 后两个是**右下角坐标**（rc 语义 `[l,t,r,b)`，最后一像素是 r-1/b-1），不是宽高；③`inset` 是**给子元素的内边距**，不是外边距，无 `height` 的子控件会被父布局拉伸填满；④**自绘控件必须用「新标签名」** —— 内建名（`Button`/`Edit`/`Option`/`Combo`/`Label`/`Progress`…）在 `UIDlgBuilder.cpp:292-352` 被拦截直接 `new` 库内类，`WindowImplBase::CreateControl()` 回调**永不触发**，自绘类拿不到创建机会（本项目用 `SkinButton/SkinEdit/SkinLabel/TabOption/GlyphCheck/RoundProgress/PartItem/TextItem/TitleLabel/WinBtn`）；⑤`font="N"` 必须先 `<Font id="N" name=... size=.../>` 声明，否则静默回退默认字体（自绘控件走 `fontsize`/`bold` 属性，不经这套）；⑥**GDI+ 抗锯齿会把 1px 描边摊成 2px 半透明**（实测 `#c9d6f2` 被画成 `#e4eaf8`，正是 50% 混白），1px 描边必须走 GDI `CreatePen(nStyle|PS_INSIDEFRAME, 1, ...)` + `HOLLOW_BRUSH` + `::RoundRect`（与 `CRenderEngine::DrawRoundRect`（UIRender.cpp:1270）同法，线全部压在 rc 内），`SmoothingModeAntiAlias` 只留给 width>1.5px 的粗线；⑦文字抗锯齿用 `CLEARTYPE_QUALITY`：老图是 WPF 无边框透明窗（灰度渲染），但 GDI 灰度 `ANTIALIASED_QUALITY` 的墨迹覆盖率只有它的 ~82%（表头 460px vs 558px），观感"发虚、不清晰"，换 ClearType 后达 96%（1294px vs 1343px），且字符 bbox 宽度与老图完全一致（主按钮 96 vs 95px、分区行 216 vs 216px）。✅ 2026-09-13

### PIT-017

- PIT-017 经典 Duilib 同组 `Option` 的 `SELECTCHANGED` **在「被取消选中」时也会发**，且与「被选中」通知的**顺序不保证**（实测：先发新选中项，再发旧项的取消）。若按 `pSender->GetName()` 直接切界面，旧项的取消通知会把刚切好的界面**又刷回上一个模式** —— 症状是「Tab 视觉上已经切换、正文内容却没变，且无任何报错」，极易误判为 `SetText/SetVisible` 没生效。正确做法：只认 `static_cast<COptionUI*>(msg.pSender)->IsSelected() == true` 的那一次通知（`CMainForm::Notify`）。验收：向窗口 `PostMessage(WM_LBUTTONDOWN/WM_LBUTTONUP)` 依次点两个 Tab，第二次点完正文必须与 Tab 一致（`tools/ui/` 截图环可复现）。✅ 2026-09-13

### PIT-018

- PIT-018 MinGW 下给 exe 嵌提权清单（GUI 启动即弹 UAC）六要点：①**GCC 14 起 MinGW-w64 不再自动链接 `default-manifest.o`** —— 本项目 GUI 原先 `RT_MANIFEST` 为空（`objdump -h` 连 `.rsrc` 节都没有），清单必须自己编，别以为"默认那份还在"；②清单只能走 `.rc`：`1 24 "SysRecoverUI.manifest"`（`24`=RT_MANIFEST，**id 必须写 1** =CREATEPROCESS_MANIFEST_RESOURCE_ID，写成别的值加载器根本不读、UAC 不弹），`windres -I src/gui x.rc -o build/x_rc.o` 再把 `.o` 和其它源文件一起交给 g++ 链接（`windres.exe` 就在 MinGW `bin/`，无需额外装）；③**清单里只写 `requestedExecutionLevel`，不要顺手补 `Common-Controls v6` / `dpiAware`** —— 本 GUI 全是 Duilib 自绘控件，受控件版本影响的只有 `SkinEdit` 内部那个原生 EDIT 子窗口，而加 `dpiAware` 会把界面从当前的 DPI 虚拟化切成真实 DPI 渲染，等于平白引入一处高 DPI 行为变更（本次需求与 DPI 无关）；即"与原 exe 相比只多提权这一处差异"；④清单方式**不改变工作目录**（同一个进程，由加载器解析），且 `CPaintManagerUI::GetInstancePath()` 取的是 exe 路径而非 CWD，故 `skin\main.xml` 定位不受影响 —— 这正是不选 `ShellExecuteW(L"runas")` 运行时自提升的主因（后者要重启进程、CWD 会变成 System32、还得自己防提权循环）；⑤Makefile 规则里**别仿写 `if not exist ... mkdir ...`**：本仓库 make 实际用的 shell 是 sh，该 cmd 写法会以 `syntax error: unexpected end of file` 失败（既有那几条从未暴露，只因目录一直存在、规则没被执行过）—— 新增输出物放到必然存在的 `build/` 根即可绕开；⑥验收：`tr -c '[:print:]\n' '\n' < dist/SysRecoverUI.exe | grep requireAdministrator` 应命中，旧产物应无（CLI 同理查 `dist/SysRecover.exe`）。✅ 2026-09-13

### PIT-019

- PIT-019 经典 Duilib `WindowImplBase` 处理 `WM_CLOSE` 时**只销毁窗口、不结束消息循环** → 进程变「无窗口僵尸」，并**长期占着单实例 Mutex**。症状三连：①`PostMessage(WM_CLOSE)` 后 `IsWindow(hwnd)` 立刻变 `False`（窗口确实没了）；②进程却一直活着（任务栏/Alt+Tab 都看不见它）；③下次启动被互斥量拦下，而 `FindWindow(类名)` 又找不到窗口 → 单实例分支落空，或走 `WaitForSingleObject(hp, 5000)` **空等满 5 秒**。真机表现：「选『使用现在新程序』后对话框先关掉、等一会儿又弹一个框」——那 5 秒空等 + 兜底提示框就是这么来的。根因：`WindowImplBase::HandleMessage` 的 `WM_CLOSE` 分支只做 `m_PaintManager` 清理与 `DestroyWindow`，**从不 `PostQuitMessage`**，于是 `CPaintManagerUI::MessageLoop()` 的 `GetMessage` 永远等不到 `WM_QUIT`。修复：在自家窗体 `HandleMessage` 里 `if (msg == WM_DESTROY) ::PostQuitMessage(0);`。**两条注意**：①局部消息循环用的对话框（`CInstanceDlg::Ask()`）**不要**跟着 PostQuitMessage —— 它靠 `m_alive` 标志退出，若在此投递 `WM_QUIT`，该消息会残留到线程队列，把随后主窗口的 `MessageLoop()` 立刻打回、程序一闪即退；②验收：`PostMessage(WM_CLOSE)` 后 500ms 内进程必须消失（未修复时实测 6s 后仍在）。✅ 2026-09-13

### PIT-020

- PIT-020 QEMU 测试台 `mk-drill.sh`/`mk-testdisk.sh` 分区表顺序坑：FAT 分区表条目从偏移 446 开始（MBR 64 字节主分区表），但脚本先用 `dd` 写 34 扇区的分区表（含 GPT 头 + 备份头），再追加写入 `grldr.mbr`（8192 字节 = 16 扇区，从偏移 0 开始）。由于 `grldr.mbr` 覆盖了前 16 扇区，**分区表条目被清零**，QEMU 识别不到任何分区 → `Cannot find GRLDR in all devices`。**修复**：先写 `grldr.mbr` 到扇区 0-15，再写分区表（扇区 16-33），这样分区表不被覆盖。✅ 2026-09-15

### PIT-021

- PIT-021 `inject-drill.py` newc cpio 头字段错位：原始版本把 `filesize`（8 字节十六进制）写到了 `mtime` 的位置（第 5 个字段），导致所有注入文件的 size 被内核解析为 0 → 注入的 wimlib-imagex/libwim 等全是空文件。正确 newc 头字段顺序（11 个字段）：`dev/ino/mode/nlink/uid/gid/rdev/mtime/filesize`（`filesize` 是第 9 个，偏移 48-55）。修复后正确解析，所有文件大小与 mtools 拷贝一致。✅ 2026-09-15

### PIT-022

- PIT-022 BG-Rescue 原生钩子机制（官方，不修改源码）：BG-Rescue 9.0.0 的 `/init`（`/etc/init.d/S90bgrescue.rc`）会自动扫描所有块设备分区，找**卷标以 `BG-Rescue` 开头**的分区，然后 source 该分区根的 `bgrescue.rc`（或 `rc.d/` 子目录）。这是 BG-Rescue 的官方扩展点，不修改其源码即可注入自定义逻辑。恢复分区必须改卷标为 `BG-Rescue...` 才能被识别。✅ 2026-09-15

### PIT-023

- PIT-023 musl time64 ABI 不兼容：BG-Rescue 9.0.0 的 musl 是 **1.1.21**（2019，无 `__stat_time64` 等符号）；Alpine 3.18 的 wimlib/fuse3 二进制是 musl 1.2.4（2023，time64）。直接加载会报 `Error relocating libwim.so.15: __fstat_time64: symbol not found`。**解法**：随包自带 Alpine 3.18 的 `ld-musl-i386.so.1`（636KB，loader+libc），用包装脚本显式调用新 loader 运行 wimlib，绕开系统老 musl。✅ 2026-09-15

### PIT-024

- PIT-024 `wimlib-imagex apply` **没有 `--ntfs` 选项**：`--ntfs` 是 Windows C API（`WIMLIB_EXTRACT_FLAG_NTFS`）的标志，Linux 版 `wimlib-imagex` 命令行工具对块设备（如 `/dev/sda2`）**自动以 NTFS 卷模式应用**，不需要也不接受 `--ntfs` 参数。传了会报 `apply: unrecognized option: ntfs`（退出码 255）。修复：去掉 `--ntfs`。✅ 2026-09-15

### PIT-025

- PIT-025 `restore.sh` 持久化日志/目标分区卸载时序三 bug：①持久化日志写完立刻卸载（`umount` 在 `echo` 之后紧跟着），只剩 1 行记录；②目标分区查完日志后**未卸载**就调 `mkntfs` → `Resource busy`；③busybox ash **无 `${PIPESTATUS[0]}`**（bash 特性），直接报错退出。修复：持久化日志写完调 `sync`（不再立刻卸载，保证完整写入）；目标分区在 `mkntfs` 前卸载；`PIPESTATUS` 改为临时文件 + `$?`。✅ 2026-09-15

### PIT-026

- PIT-026 `exec /bin/sh -c '...'` in /init hangs：BG-Rescue 的 busybox ash 里 `exec /bin/sh -c 'complex script'` 会因 stdin 阻塞挂死。必须 `</dev/null` 重定向 stdin。✅ 2026-09-16

### PIT-027

- PIT-027 复杂 /init 脚本（含循环/条件/mount）在 QEMU 里挂死：在 busybox ash 的 /init 上下文中，复杂脚本（循环+条件+mount 组合）可能因解析或资源问题挂死。根因未完全定位，规避方案：用 S99zjrestore（/etc/init.d/ 独立脚本）替代 /init 修改。✅ 2026-09-16

### PIT-028

- PIT-028 `inject-drill.py` 替换 /init 条目产生损坏 cpio：替换 /init 入口点的 cpio 注入方式会产生不可解析的 cpio（offset 7400 parse error）。规避：不替换 /init，改用 /etc/init.d/ + rcS 机制。✅ 2026-09-16

### PIT-029

- PIT-029 Python 读写整个 2GB 磁盘镜像（`bytearray(open(...).read())`）损坏 FAT32：Python 一次性读取整个磁盘镜像到内存会破坏 FAT32 文件系统。必须用 `dd` 或小缓冲区写入做分区表修补。✅ 2026-09-16

### PIT-030

- PIT-030 wimlib-imagex wrapper 需要 ZJTOOLS 路径在 exec 时有效：wrapper 脚本用 `$ZJTOOLS/ld-musl-i386.so.1`，如果 mount 被卸载后 exec，wrapper 找不到 musl loader。解法：S99zjrestore 在 exec 前将 zjtools 库拷贝到 /tmp（已落地）。✅ 2026-09-16

### PIT-031

- PIT-031 mtools `mcopy -i IMG@@OFFSET` 需要 `-o` 覆盖标志：对已有文件的 FAT32 分区做 mcopy 时不带 `-o` 会静默失败。✅ 2026-09-16

### PIT-032

- PIT-032 `mk-initramfs.sh` 用 wrapper 脚本覆盖了 wimlib-imagex ELF 二进制：`base/zjtools-wimlib-imagex`（494字节 shell 脚本）被拷贝为 `/usr/bin/wimlib-imagex`，覆盖了 Alpine 的 62760字节 ELF 二进制。S99zjrestore 拷贝该"假二进制"到 `.real`，musl loader 尝试执行 shell 脚本 → `Not a valid dynamic program`。**修复**：ELF 二进制放 `/usr/bin/wimlib-imagex.real`，wrapper 脚本放 `/usr/bin/wimlib-imagex`，S99zjrestore 拷贝 `.real` 文件。✅ 2026-09-16

### PIT-033

- PIT-033 busybox blkid 不支持 `-s PART_ENTRY_OFFSET`：BG-Rescue 的 busybox blkid 可能不识别 `-s PART_ENTRY_OFFSET -o value` 选项，导致分区 offset 匹配失败。Fallback：从设备名推断分区号，用 `dd`+`od` 直接读 MBR 分区表获取 LBA 起始扇区。✅ 2026-09-16

### PIT-034

- PIT-034 UEFI 下 grldr.mbr 无法加载（0xc000007b）：`grldr.mbr` 是16位实模式 bootsector，Windows boot manager 在 UEFI 模式下无法执行它（错误 `0xc000007b`——文件存在但格式无效）。症状：BCD 条目创建成功（菜单出现），但选择后报错"所需文件丢失或包含错误"。**临时规避**：safety check 检测 GPT GUID 并拒绝还原，提示"当前版本仅支持 MBR/BIOS 引导，UEFI 支持开发中（Phase 3）"。**路线**：先把 MBR/BIOS 做通，UEFI 引导（GRUB2 + `grub.cfg` + `bootx64.efi`）在 Phase 3 实现，不放弃。✅ 2026-09-16

### PIT-035

- PIT-035 契约文件跨分区导致 Linux 找不到任务：`_zjresy*.log` 写**目标分区根**（主契约），`restore-task.conf` 写**软件目录**（副契约），二者常在不同分区（日志在 C:、软件在 D:\dist）。原 `S99zjrestore` 只在「日志所在分区根」找 conf → 找不到 → `zjrestore-lite.sh` 判"未找到任务文件"退出 → 停在 `rescue:/#`（症状：还原环境起来后无任何动作）。**修复**：①Windows 侧额外把 conf 写到目标分区根（与日志同处）；②S99zjrestore 独立搜索各分区（含子目录 `-maxdepth 4`）找 conf；③zjrestore-lite.sh 的 `get_task` 在 conf 缺失时回退到日志字段（日志已含全部所需键）。✅ 2026-09-16

### PIT-036

- PIT-036 GRUB4DOS 菜单中文乱码：GRUB4DOS 显示中文需额外加载字体文件（`fontfile` + 中文码表），本项目未随包分发字体，menu.lst 写中文（UTF-8 或 GBK）一律显示为乱码。**规避**：menu.lst 一律用 ASCII 文本（`SysRecover Restore Environment`）。若要中文需另做字体打包。✅ 2026-09-16

### PIT-037

- PIT-037 `NeedsInstall` 短路导致修复不生效：原还原流程写 `if (NeedsInstall(...) && !InstallBootLayer(...))`——四项检查全过就**跳过安装**，于是新版本修正过的 `menu.lst`/`initramfs`/脚本**不会随还原刷新**（症状：改了菜单/脚本，VM 上表现仍是旧的）。**修复**：还原前**无条件** `InstallBootLayer`（幂等，BCD 条目已存在则跳过创建），确保引导文件随本版本刷新。✅ 2026-09-16

### PIT-038

- PIT-038 BG-Rescue 启动阶段有 ~8 次交互式选择，每次 `read -t 15` 阻塞 15 秒：来自 BG-Rescue 原生 init 脚本——`/etc/init.d/S20keymap`（选键盘布局）、`S25font`（选字体）、`S30modules`（是否加载全部模块）、`S40network`（是否启用网络）、`S60lvm2`（是否启用 LVM），以及 `/etc/inittab` 的 `tty2/3/4::askfirst`（3 次"Please press Enter to activate this console"）。这些脚本都读 `/etc/boot.config` 的 `BGKEYMAP/BGFONT/...` 变量（设 `"0"` 即跳过），但该文件由 `/init` 在启动时无条件用空值覆盖，且交互变量不跨脚本传递（rcS 每个脚本都在子 shell/子进程里跑），**无法用配置消除**。**解法**：在 initramfs 里注入同名覆盖脚本（`bootfiles/zj-init/`，构建时 `inject-drill.py` 的 drop 参数先删原文件再加覆盖版），脚本只保留必要逻辑（如 S30modules 保留基础模块加载、去掉"加载全部"询问），并注入精简 `inittab`（只留 `tty1::respawn`）。✅ 2026-09-16

### PIT-039

- PIT-039 Duilib `CSkinLabelUI`/`TextIn` 是**单行绘制**（`TextOut`），文本里的 `\n` 不会换行（会画成缺字符方框）。多行提示必须拆成多个 `SkinLabel`，逐行 `SetText`（见 `CConfirmDlg::InitWindow` 按 `'\n'` 拆分到 `DlgMsg1/2/3`）。✅ 2026-09-16

### PIT-040

- PIT-040 `zjrestore-lite.sh` 用 offset 匹配目标分区永远失败：`blkid -s PART_ENTRY_OFFSET` 返回 **512 字节扇区数**，而契约 `target_part_offset`（`WriteRestoreLog`/`WriteRestoreTask`）写的是**字节数**，两者恒不相等 → 判"未找到目标分区"退出 → 停在 `rescue:/#`。**正解**：恢复日志 `_zjresy*.log` 本来就写在**目标分区根**，所以「找到日志的分区就是目标分区」——`S99zjrestore` 把该设备作为 `$1` 传给脚本（对齐生产 `restore.sh` 的定位方式），不再做 offset 比较；脚本保留 offset 回退但改为 `_sec * 512`。✅ 2026-09-16

### PIT-041

- PIT-041 BG-Rescue `/init` 固定 15 秒 USB 枚举等待：`/init` 里 `DELAY=15` + `echo "Waiting for $BGUSBDELAY seconds..."` + `sleep $BGUSBDELAY`，而 `BGUSBDELAY` 只能由「卷标含 BG-Rescue」分区上的 `bgrescue.cfg` 覆盖（本项目不改卷标，故无效）。**解法**：注入修改版 `/init`（`bootfiles/zj-init/init`，`DELAY=0` 并删掉该 echo+sleep），构建时用 `inject-drill.py` 的 drop 参数先删原 `/init`。注意 PIT-027/028 的警告源于当年**有 bug 的注入器**（PIT-021 头字段错位），现注入器已修好，替换 `/init` 安全；替换后务必 `parse-initramfs.py cat init` 复核内容再上机。✅ 2026-09-16

### PIT-042

- PIT-042 诊断 Linux 还原阶段"无动作"：rescue 内核**不向串口/VGA 输出内核日志**（PIT-014），但 **shell 里的 stdout 是可见的** —— 故 `S99zjrestore`/`zjrestore-lite.sh` 的 `say()` 必须同时 `echo` 到控制台（不只写 `/tmp/zjrestore.log`），失败时用户在 `rescue:/#` 能直接看到卡在哪一步；`/tmp` 是 RAM，重启即失，需要长期留痕时再写目标/镜像分区。✅ 2026-09-16

### PIT-043

- PIT-043 rescue 内核**存储驱动覆盖有限**，VMware LSI SCSI 硬盘认不到：内核 `5.15.12-64bit (bg@rescue)` 内建 **ahci / ata_piix(IDE) / nvme / usb-storage / sd**（够用真实 PC），但**没有 mptspi/mptsas（VMware LSI Logic SCSI）、virtio_blk、megaraid**。症状：`/proc/partitions` 除表头外为空、`/sys/class/block` 只有 loop0-7，`list_parts` 空 → 找不到 `_zjresy*.log` → 落 shell（**不是挂载问题**，是设备根本没出现）。内核 `CONFIG_MODULE_UNLOAD=y`（**支持模块**）但 initramfs **不含任何 .ko**（`usr/lib/modules` 是空目录，`/etc/modules` 仅注释）——BG-Rescue 的模块在其 ISO 的 squashfs 里，我们只用 initramfs 故拿不到。vermagic = `5.15.12-64bit SMP mod_unload `（BG-Rescue 自编，只有它自己的模块能加载）。**规避**：VM 磁盘控制器改用 **SATA(AHCI) 或 NVMe**（内核都支持）；**正解**：换 Alpine 底座（PIT-044）。✅ 2026-09-16

### PIT-044

- PIT-044 **救援底座换 Alpine linux-lts**（2026-09-17，根治 PIT-043）：BG-Rescue 内核的 `.config` 可从 vmlinuz 里提取（`IKCFG_ST`…`IKCFG_ED` 之间是 gzip 的 `.config`），证实 54 个 SCSI HBA 驱动**全部 `is not set`**、`=m` 模块数 **0** → 所谓"支持 SCSI Disk"只指通用 `sd` 层，**没有 HBA 驱动**，VMware LSI/服务器 RAID 一律认不到盘。Alpine 的 `linux-lts` 把全套 HBA 编成模块：`mpt3sas`/`mptspi`/`mptsas`/`megaraid_sas`/`aacraid`/`hpsa`/`arcmsr`/`vmw_pvscsi`/`virtio_blk`/`virtio_scsi`/`usb-storage`/`uas` + `ntfs3`/`fuse`/`vfat`/`exfat`。**组装方式**：Alpine 包是 **tar.gz（apk）**，可在 **Windows 上直接解包**，无需 Linux 环境 → `tools/vmtest/build-alpine-initramfs.py` 从 apk 组装（busybox-static + ntfs-3g/mkntfs + wimlib + util-linux blkid + musl + 裁剪后的 443 个模块），内核用 `boot/vmlinuz-lts`。附带收益：Alpine 内核内建 `SERIAL_8250_CONSOLE`/`FRAMEBUFFER_CONSOLE`，**PIT-014 的"控制台零输出"问题也随之消失**。✅ 2026-09-17

### PIT-045

- PIT-045 Windows 上打包 initramfs **必须显式带权限位**：Windows 不跟踪 Unix 执行位（`os.stat().st_mode & 0o111` 恒为 0），若用本地文件权限推 mode，cpio 里所有文件都会是 0644 → 内核执行 `/init` 报 `Failed to execute /init (error -13)`（EACCES）→ `Kernel panic: No working init found`。解法：解 apk 时用 tar 成员自带的 `m.mode` 记录到 `MODES{路径:权限}`，打包时按此写 cpio；自行生成的脚本显式标 0755。**另**：`/init` 的 `#!/bin/sh` 要求 `/bin/sh` 在 exec 前就存在 → 镜像里预置 `/bin/sh -> busybox` 符号链接（其余 applet 由 `/init` 里 `busybox --install -s /bin` 运行时生成）。✅ 2026-09-17

### PIT-046

- PIT-046 apk 内大量文件是**硬链接**：Alpine 的 wimlib 包中 `wimlib-imagex`/`wimapply`/`wimcapture`… 全是硬链接到 `wimappend`（tar `type '1'`，`m.isfile()` 为 False）。解包函数只处理普通文件/符号链接会**静默漏掉它们**（症状：`wimlib-imagex: not found`、只有 `wimappend` 落地）。解法：`m.islnk()` 时建**相对符号链接**（`os.path.relpath(m.linkname, dirname(m.name))`）——cpio 无硬链接概念，符号链接既能省空间，`argv[0]` 仍是原名，wimlib 多调用程序按名字分派行为不变。✅ 2026-09-17

### PIT-047

- PIT-047 `busybox --install -s /bin` 会**遮蔽真工具**：busybox 自带简化 `blkid`，装到 `/bin/blkid` 后排在 PATH 的 `/bin` 里，抢在 util-linux 的 `/sbin/blkid` 前面 → `-s TYPE -o value` 返回空 → 文件系统类型判定失败、挂载走错分支（症状：vfat 分区报 `mount ... failed: No such device`）。解法：`--install` 之后 `rm -f /bin/blkid`。**另**：initramfs 根里默认**没有 `/proc` `/sys` 目录**，必须 `mkdir -p` 后再 `mount`，否则 `mount -t proc proc /proc` 失败（`/sys/class/block` 空 → 一个分区都枚举不到）。✅ 2026-09-17

### PIT-048

- PIT-048 引导修复不再依赖 `ms-sys`（Alpine 仓库无此包）：我们只格式化**分区**、不动磁盘 MBR 引导代码，所以关键是恢复分区 PBR。做法：`mkntfs` **之前** `dd` 备份原 PBR 512 字节，**之后**把微软引导代码段（偏移 `0x54..0x1FD`，426 字节）写回——等价 `ms-sys --ntfs` 的 boot code 部分，且 BPB 仍是 mkntfs 的正确值。**移植前先验 OEM 名**（偏移 0x03 8 字节是否为 `NTFS    `），否则会把 FAT 引导代码写到 NTFS 上。`ms-sys` 存在时仍优先走它（兼容旧底座）。✅ 2026-09-17

### PIT-049

- PIT-049 PBR 移植 `dd` **必须同时给 `skip=84` 和 `seek=84`**：只写 `seek=84 count=426` 会从源文件的**偏移 0** 读，把原 PBR 开头的 BPB 写到新 PBR 的 0x54，引导代码整体**错位 78 字节**——症状是开机黑屏、左上角光标闪烁（BIOS 已交出控制权，PBR 执行到垃圾指令）。校验：把 `dd` 后的 PBR 与未动过的同型 NTFS 分区 PBR 对比，0x54 处应是 `fa 33 c0 8e d0 bc 00 7c`（而不是 `eb 52 90 4e 54 46 53`）。已加回归测试 `tools/vmtest/test-pbr-transplant.sh`。✅ 2026-09-17（用户实测踩到）

### PIT-050

- PIT-050 引导代码**不能以「格式化前备份的原 PBR」为源**（会自我复制污染）：`mkntfs` 会给新分区写一份 **ntfs-3g 自带的引导代码**（特征：有 `BOOTMGR is compressed` 但**没有** `BOOTMGR is missing`），它**不能引导 Windows**。一旦某次还原后分区停留在这个状态，下一次还原的"原 PBR 备份"就是这份非引导代码 → 每次都写回它 → 永远黑屏光标闪（实测：两次还原后 sda1 PBR 的 0x54 区与真 Windows 差 140 字节）。**修复**：随包内置 `bootfiles/ntfs-boot-code.bin`（426B，偏移 0x54..0x1FD，取自同机未动过的真 Windows 分区 PBR），还原时优先写它；只有备份 PBR 通过「OEM=`NTFS    ` + 含 `BOOTMGR is missing`」双重校验时才用备份。**诊断技巧**：`mkntfs -f` 后根目录是空的（只有 `$MFT`/`$Bitmap` 等 `$` 系统元文件），所以「根目录里出现 `Windows`/`Users`/`bootmgr`」= apply 确实写了；配合 `$Bitmap` 的已用簇数可定性。✅ 2026-09-17（用户实测踩到）

### PIT-051

- PIT-051 **万能镜像的 C: 常不含 `\Boot`（BCD）**，apply 完也起不来：Win10 官方安装会把引导文件放 C:\（BIOS）或 ESP（UEFI），而"万能/GHO/ESD 系统镜像"作者常常只打包系统分区内容，引导文件靠部署工具生成。我们只 apply 镜像 → `C:\bootmgr` 有、`C:\Boot\BCD` 没有 → 无法启动（PE 里 `bcdboot` 一跑就好）。**修复（BIOS 路径）**：**Windows 暂存阶段**（本机还在跑 Windows）用 `bcdedit /store <copy>\BCD` 把 `{bootmgr}`/`{default}` 的 `device`/`osdevice` 改成**可移植的 `boot`**、`{default} path \Windows\system32\winload.exe`，连同 `C:\bootmgr` 一起复制到 `D:\ZJRESTORE\bootfix\`，再用 libwim 打成 `bootfix.wim`；**Linux 侧**在 apply 完主镜像后**再 apply 一次 `bootfix.wim`**（`/init` 扫各分区找它），把 `\bootmgr`+`\Boot\BCD` 写进目标分区。原因：Linux 侧没有 bcdboot，而 BCD 是注册表 hive，不好凭空生成。UEFI 路径（写 ESP `\EFI\Microsoft\Boot\BCD` + `bootx64.efi`）留待 Phase 3。**踩坑**：`C:\Boot\BCD` 在运行中的 Windows 里被当作注册表 hive 挂着（`HKLM\BCD00000000`），文件**独占锁定**，`CopyFile` 必失败（`copy FAIL C:\Boot\BCD`）；必须改用 `bcdedit /export <目标文件>` 导出（官方方式，导出的就是可用作 `\Boot\BCD` 的 store），再 `bcdedit /store` 改成 `device boot`。**同理**：内置引导代码 `ntfs-boot-code.bin` 只是 426B 代码段，写回时**不能**再 `skip=84`（那是给完整 512B PBR 用的）——曾因此把代码段又跳了 84 字节，而校验用同一 skip 造成"假通过"（实测 PBR 0x54 变成 `e13b060b`）。回归测试 `tools/vmtest/test-pbr-transplant.sh` 覆盖两条路径。**再踩坑**：Linux 侧**不能用 wimlib apply 写补全包** —— wimlib 以「NTFS 卷」模式 apply 时**不覆盖已存在文件**，而镜像里通常已有 `\bootmgr` → `Can't create "/bootmgr" in NTFS volume: File exists` → rc=46（`WIMLIB_ERR_NTFS_3G`），`\Boot` 只建出空壳。改为：Windows 侧产出 `ZJRESTORE\bootfix\` **目录**（不打 WIM），Linux 侧 mount 目标分区（`ntfs-3g -o force`）后 **`cp -rf` 覆盖写入**。启动菜单等待也顺手设 `{bootmgr} timeout 0`。✅ 2026-09-18（QEMU 演练验证：`install bootfix files` → `bootfix installed; target root: ... Boot ... bootmgr`；`PBR boot code written+verified (0x54 = fa33c08e)`）

### PIT-052

- PIT-052 ⚠️**本节"不认 mkntfs"的结论已被 PIT-053 推翻**（保留作历史）：**Windows 的 BIOS 引导代码不是 512 字节**（开机黑屏光标闪的根因之一）：引导代码跨 **9 个扇区**——第一扇区 0x54..0x1FD 的代码段 + **扇区 1..8 的后续代码**；代码开头把后面 16 个扇区读进内存，再 `jmp 0x226` 跳到扇区 1 继续。`mkntfs` 只写第一扇区（自带一份非 Windows 代码），仅移植第一扇区的 426 字节 → 扇区 1..8 全 0 → `jmp 0x226` 跳进空白 → 死循环（**无任何报错**，只有光标闪）。补齐扇区 1..8 后**仍然黑屏**：阳性对照（QEMU 用真 Windows MBR 启动活动分区）证明同一份代码在 **Windows 自己格式化的 NTFS**（同机数据盘 sda2）上能正常跑（找到根目录、报 `BOOTMGR is missing`），在 **mkntfs 造的分区**上却在解析 `$MFT`/根目录时静默跳 BIOS boot-failure 入口 → **引导代码不认 mkntfs 的布局**。**修复**：**不格式化目标分区**，保留其原有 NTFS（连它的 Windows 引导区一起保留），用 `ntfs-3g` 挂载后 `rm -rf` 清空文件，再 `wimlib apply` + `cp` bootfix；目标非 NTFS 才退回 mkntfs。另内置 `bootfiles/ntfs-boot-cont.bin`（扇区 1..8）用于修复被上次失败还原写坏的引导区。**排查手法**（可复用）：`qemu-img create -f qcow2 -b <vmdk> ovl.qcow2` + `qemu-io -c "write -s file.bin <off> <len>"` 做免破坏试验，`-monitor tcp:...` + `screendump` 抓 VGA 屏。✅ 2026-09-18

### PIT-053

- PIT-053 **mkntfs 快速格式化完全可用，PIT-052 是误诊**（2026-09-18 QEMU 实证）：用一个「mkntfs 分区 + 完整微软引导区（扇区 0 代码 426B + 扇区 1..8 共 4096B）+ 根目录**故意不放 bootmgr**」的靶子，用 GRUB4DOS `chainloader` 直接引导它，屏幕正确显示 `BOOTMGR is missing / Press Ctrl+Alt+Del to restart` —— 证明微软引导代码**能**解析 mkntfs 卷并找到根目录。所以目标分区**默认 mkntfs 快格**（`ZJ_TARGET_MODE=format`），不再需要"保留原 NTFS 只清空"那套绕路（它还会带来：残留文件→wimlib rc=46、FUSE 清空 20s 卡顿）。PIT-052 当初失败的真实原因是我们自己移植引导区时的三个 bug 叠加：①**漏写扇区 1..8**（只在 `REPAIR_BOOT` 的"保留 NTFS"分支写，`mkntfs` 分支从来没写）；②`dd skip=84` 错位（PIT-049，当时未发现）；③`hidden sectors` 写错（`mkntfs --partition-start` 不对时引导代码按错误绝对偏移读盘）。另：Alpine 的 `mkntfs` 自带的是**占位代码**（扇区里是 `This is not a bootable disk...`），必须整体换成微软引导区。**回归测试**：`tools/vmtest/pbr-probe.py`（造探针 → chainload → `memsave 0xb8000` 读 VGA 文本 → 断言 `BOOTMGR is missing`，退出码 0/1）。⚠️ 读屏**不要**用 monitor 的 `xp`（逐字符回显 + 转义序列，解析不可靠），用 `memsave <addr> <len> <file>`。✅ 2026-09-18

### PIT-054

- PIT-054 `bcdedit /export` 会把**暂存阶段加的 GRUB4DOS 恢复条目**和 `{bootmgr} bootsequence` 一起导出进 bootfix 的 BCD，导致还原后的目标机出现**两个启动菜单**（一个是真 Windows，另一个是恢复环境的中文描述 → 显示成一串方框），而且 `bootsequence` 残留会把重启**再带回 GRUB4DOS/Linux**（症状：还原完成重启 → 选"Windows"却进 `#` shell，见用户 1111.png）。`PrepareBootFixFiles` 原来只删了 `{memdiag}`。**修复**：导出后对目标 store 再执行 `deletevalue {bootmgr} bootsequence` + `displayorder {恢复GUID} /remove` + `delete {恢复GUID} /f`，只留真正的 Windows 条目（`{memdiag}` 删除也补 `/f`）。✅ 2026-09-19（待用户回归确认）

### PIT-055

- PIT-055 Alpine 救援层在 **VMware/QEMU 虚拟 NVMe** 上还原大镜像时，进度每 N% 卡满 **30s**，dmesg 报 `nvme nvme0: I/O N QID M timeout, completion polled`（实测 4982MiB 镜像）。**真正的根因**是虚拟 NVMe **偶发"完成中断丢失"**：设备其实**已把完成项写进 CQ**，但驱动没收到中断 → 要等满 `io_timeout`（默认 **30s**）才在超时处理里轮询 CQ 并发现完成项（内核原文："the device did indeed post a completion queue entry … but the driver believes it was never notified via interrupt"）。所以卡顿时长 = io_timeout，**与脏页/回刷无关**——压小 `dirty_ratio` 无效（实测只是把首次卡顿从 14% 提前到 2%，没减时长，已回退）。**修复**：把 `nvme_core.io_timeout`（单位=**整数秒**；`#define NVME_IO_TIMEOUT (nvme_io_timeout * HZ)`，故**最小只能到 1s，设不了 500ms**）从 30 降到 **1** —— 三处同时上：`modprobe nvme-core io_timeout=1`（见 `bootfiles/alpine/init`）、写 `/sys/module/nvme_core/parameters/io_timeout`（见 `init` 与 `zjrestore-lite.sh`）、内核 cmdline `nvme_core.io_timeout=1`（见 `src/boot/grub.cpp`）。轮询几乎立刻就在 CQ 找到完成项，卡顿 30s→1s；单个 I/O 正常远小于 1s，不会误判（真超时仍走内核既有 abort/reset，与超时值无关）。⚠️ 方向别搞反：**调大** io_timeout 只会让卡顿更久。⚠️ 卡顿是**被内核扣住的那个 I/O 请求**阻塞了写线程，所以"先跑到 100% 再检查"不可行（内核不会放行该请求）；总浪费 = 丢中断次数 × io_timeout，唯一杠杆就是超时值。🚧 待实测（属模拟器竞态，真实 NVMe 少见）。**测试 VM**：`D:\Software\System\_VMware\VMdisks\Win10New`（NVMe，**只有快照1**，旧的 LSI SCSI VM 与快照3 已不存在）。

### PIT-056

- PIT-056 **用自己的热备份镜像还原后黑屏起不来 —— 元凶是排除了注册表事务日志**（2026-09-19 定位）：第三方镜像还原正常，唯独我们自己 backup 出来的镜像还原后黑屏。逐层排查：①镜像完整（用 libwim 列过 `\bootmgr`/`\Boot\BCD`/`\Windows\System32\winload.exe`/`ntoskrnl.exe`/`config\SYSTEM` 都在）；②还原流程正常（Linux 日志 `apply rc=0`、`bootfix installed`、`boot region OK (0x54=fa33c08e)`、`RESTORE DONE`）；③读 MBR 确认 **p1 就是活动 NTFS 分区**、还原后 `\Boot\BCD` = 20480B（bootfix 导出的可移植 BCD，非镜像自带的 24576B）→ 引导链完整。**真正原因**：热备（VSS）时 `\Windows\System32\config\SYSTEM` 等 hive 常处于**"脏"状态**（实测 base block 里 `primary_seq=130 != secondary_seq=129`），Windows 开机必须靠 `.LOG1/.LOG2` 把 hive 恢复一致；而 `exclude.cpp` 的默认清单**排除了 `\Windows\System32\Config\*.log*`、`*.regtrans-ms`、`*.TM.blf`、`\Users\*\NTUSER.DAT*.log*`** → 日志随包丢了 → hive 无法恢复 → **开机黑屏**。微软默认 `WimScript.ini` **不排除**这些日志。**修复**：从默认排除清单里删掉这 6 条，随包捕获日志（只有几 MB）。✅ 2026-09-19（**用户实测确认**：修复后重新热备份 + 还原，系统正常进入；排查手法：`wimlib_extract_paths` 取 hive → 读 REGF base block 的 `primary_seq/secondary_seq` 判脏；`bcdedit /store` 读还原后的 BCD）

### PIT-057

- PIT-057 "还原完黑屏起不来"可能是**镜像本身是"写入未完成"的半截文件**（2026-09-19，承 PIT-056）：用户用刚备份的 `backup-20260919-124942.esd` 还原，症状是 GUI "开始恢复系统"按钮**是灰的**（切一下模式又能点），点了之后重启卡在 `#`。VM 磁盘取证（QEMU `-snapshot` 挂 VMDK + 临时"检查器" init 把 `zjrestore-debug.log` 打到串口）看到 Linux 侧 `wimlib-imagex apply` 原文：`[WARNING] The WIM_HDR_FLAG_WRITE_IN_PROGRESS flag is set ... a process may have crashed while writing the WIM` → `rc=84 WIMLIB_ERR_WIM_IS_INCOMPLETE`（= "written by a program that was terminated before finishing"）。即**上次备份中途退出/中断**，WIM 头 "写入中" 标志未清、文件是半截。而**暂存阶段从不校验镜像** → 坏镜像被暂存进任务 → 重启后 Linux 先把目标分区 mkntfs 了，apply 才发现装不上 → 既没系统又没引导 → 黑屏。**修复**：①新增 `WimEngine::Probe()`（`wimlib_open_wim(WIMLIB_OPEN_FLAG_CHECK_INTEGRITY)` + 检查 `wim_info.write_in_progress` / `image_count`），`StageRestore` 在**做任何事之前**先校验，坏镜像直接拒绝（退出码 5 + "请重新备份"）；②备份成功后也 Probe 一次自己的产物；③GUI 新增 `m_imageOk`（镜像可读才置真），读失败时主按钮保持灰 —— 修掉"切下模式就能点坏镜像"的怪象。**取证手法**（可复用）：把检查器 init 写进 `bootfiles/alpine/init` 临时构建 initramfs，`qemu-system-x86_64 -kernel vmlinuz-zjrestore -initrd <insp> -append console=ttyS0 -drive file=<vmdk>,format=vmdk,if=virtio -snapshot -nographic`（`-snapshot` 保证不写真实盘），检查器里 mount 只读后把目标/数据分区的日志 `while read` 逐行 echo 到串口。✅ 2026-09-19（Probe 已用"人为置 `WIM_HDR_FLAG_WRITE_IN_PROGRESS`"的镜像验证：暂存被拒、退出码 5）

### PIT-058

- PIT-058 **GRUB4DOS 的 `grldr` 必须在分区根目录，做不到"根目录零文件"**（2026-09-19）：官方 README 原文 `The bootstrap code of GRLDR.MBR only finds GRLDR file in the root dir of a partition.`；且 `grldr` 只在 `/menu.lst`、`/grub/menu.lst`、`/boot/grub/menu.lst` 找配置（`grldr.mbr` 虽可由 BCD `path` 指向子目录，但用户最终决定不折腾，直接放根目录）。**最终布局**：根目录放 `grldr` + `grldr.mbr` + `menu.lst` 三个隐藏+系统文件，其余全部进 `D:\ZJRESTORE\`（`boot\`/`scripts\`/`logs\`）——即"一个文件夹 + 根目录三个隐藏小文件"。**诊断日志统一写 `D:\ZJRESTORE\logs\`**（`ZJRDIR`/`diag_dir`，不散在 C:/D: 根目录；出故障时让用户打包这个子文件夹发回）。**还原成功后 `cleanup_after_success` 把上面全部删掉（含 `logs\`），失败时全保留**（便于排错/重试）。⚠️ 想根目录零文件只能改 `grldr` 内置菜单或自写启动扇区，都不划算。✅ 2026-09-19（用户实测：热备份+还原正常）

> **备选方案（已验证可用，暂不采用）**：`grldr.mbr` 也可放进 `D:\ZJRESTORE\`，BCD 写 `path \ZJRESTORE\grldr.mbr`（**用户已在 VM 实测通过**），这样数据盘根目录只留 `grldr` + `menu.lst` 两个隐藏文件。当前为最大兼容性仍按上面"根目录三文件"布局；如日后需要更少根目录文件，可直接切回该方案（改 `DeployGrldr`/`BcdCreateBootsector` 两处即可）。

### PIT-059

- PIT-059 **布局优化：救援改放目标盘（C:），日志放软件目录**（2026-09-19，用户要求）——不再往数据盘（D:/E:）根目录塞任何东西。①**救援文件**（`grldr`/`grldr.mbr`/`menu.lst` + `ZJRESTORE\{boot,scripts,bootfix}`）部署到**目标分区**（= 本次要被格式化的那块盘，通常 C:）：GRUB4DOS 在格式化前已把内核/initramfs 读进内存、bootfix 也被 `/init` 在 mkntfs 前拷进 `/tmp`，所以格式化目标不影响还原；BCD 条目 `device partition=<目标盘>:` + `path \grldr.mbr`。②**诊断日志写软件目录**（`<exeDir>\logs\`，`restore-task.conf` 新增 `software_dir=`，Linux 侧 `find_soft_dir` 定位）：**成功也保留**，用户直接在软件目录里打包发回；软件在目标盘/只读介质上时回退到 `<数据盘>\ZJRESTORE`（GUI 先弹确认）。③**引导期日志**：`/init` 在跑还原脚本前把引导阶段日志写一份到**目标分区根** `zjrestore-boot.log`；脚本一启动就删掉它（此后日志进软件目录）。**它若还在 = 引导阶段没走完**，让用户把该文件发回即可排错。④成功还原会把目标分区格式化 → 目标盘上的救援文件/引导日志**自然消失**；`cleanup_after_success` 只清旧版本遗留在数据盘根目录的引导文件（**不碰软件目录日志/镜像**）。✅ 2026-09-19（**用户实测确认**：VM 中热备份 + 还原均正常；目标盘部署生效、D:/E: 根目录干净、日志落软件目录）

### PIT-060

- PIT-060 **UEFI/GPT 分支：内核 EFI stub + 往 NVRAM 写「固件启动项」（Boot####），既不用 GRUB2 也不用 loader**（2026-09-19）：`bootfiles/vmlinuz-zjrestore` 是带 EFI stub 的 PE（`MZ` + PE + subsystem=10=EFI_APPLICATION），**UEFI 固件可直接加载它**，`initrd=` 走内核命令行。QEMU(OVMF) 实测全链（`tools/vmtest/uefi-smoke.ps1`，PASS：`EFI stub: Loaded initrd from command line option` → Alpine 救援 `/init` → `SR: modules loaded`）。
  - ❌ **弯路：BCD `/application bootapp` 不行**。bootmgr 的 bootapp 只接受 subsystem=16 的 bootmgr 私有程序，加载我们的 subsystem=10 内核 → **`0xc000007b STATUS_INVALID_IMAGE_FORMAT`**（VM 实测：启动菜单里选条目 → 卡在固件 `Attempting to start up from: Windows Boot Manager...`，还原从未开始、目标分区完好）。另：`device` 也必须写 `boot`（`partition=X:` 的盘符在启动环境里解析不了）。
  - ✅ **正解（成熟法，同 Rufus / `efibootmgr` / `bcfg`）**：用 `GetFirmwareEnvironmentVariableW` / `SetFirmwareEnvironmentVariableW`（kernel32；写前必须 `AdjustTokenPrivileges` 启用 **`SeSystemEnvironmentPrivilege`**，管理员默认持有但未启用）往 NVRAM 写 `Boot####` + 插 `BootOrder`（命名空间 `EFI_GLOBAL_VARIABLE = {8BE4DF61-93CA-11D2-AA0D-00E098032B8C}`），**由固件直接加载内核**。
    - `EFI_LOAD_OPTION` 布局：`u32 Attributes(=1 即 ACTIVE)` + `u16 FilePathListLength` + `Description(UTF-16, 含 NUL)` + `FilePathList` + `OptionalData`。`FilePathList` 用**短格式**（固件自行展开，不需要 PCI 前缀）= `HardDrive(42B: part#, 起始 LBA, 扇区数, GPT PartitionId, mbrType=2, sigType=2)` + `FilePath(4+2*(len+1))` + `End(7f ff 04)`；GPT 信息从 `\\.\X:` 的 `IOCTL_DISK_GET_PARTITION_INFO_EX` 取（扇区大小取 `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX`）。
    - **`OptionalData` 就是该镜像的 `LoadOptions`**：内核 stub 的 `efi_convert_cmdline()` 按 **`efi_char16_t*`（UTF-16LE）** 读取 → 命令行**必须 UTF-16LE**（内容：`initrd=\EFI\ZJRESTORE\initramfs-zjrestore.cpio.gz console=tty0 nvme_core.io_timeout=1 zjre=1`，`zjre=1` 是判归属的标记）。
    - QEMU 实证（`tools/vmtest/uefi-bootentry-smoke.ps1`）：固件日志 `BdsDxe: loading Boot0003 "ZJTEST" from .../vmlinuz-zjrestore.efi` → **固件确实从 `Boot####` 直接加载启动了我们的内核**。
    - 代码：`src/boot/uefi.cpp`（`InstallUefiBootEntry` / `RemoveUefiBootEntry` / `SetUefiBootNext` / `UefiBootEntryExists`）；写变量优先 Win8+ 的 `SetFirmwareEnvironmentVariableExW`（显式属性 `0x7` = NON_VOLATILE|BOOTSERVICE|RUNTIME，保证**重启后仍在**），Win7 回退普通版。
    - **常驻 + 单次**：条目挂在 **`BootOrder` 末尾**（默认仍进 Windows；开机启动菜单里能选到 → **Windows 蓝屏/引导损坏也能进**，这是用户明确要求）；还原暂存用 **`BootNext`** 做**单次**启动（不依赖顺序）。GUI 按钮 = `安装启动还原` / `删除启动还原`。
    - 安装时顺手清旧版残留：`bcdedit /delete {GUID} /f` + `deletevalue {bootmgr} bootsequence`（否则 Windows 启动菜单里会留一个必然报 `0xc000007b` 的条目）。
  - **ESP 挂载**：`mountvol X: /s`（用完 `/d`）。**还原后修 UEFI 引导**：`bcdboot <目标>:\Windows /s <ESP>: /f UEFI`。**Linux 侧**：`pt_type=gpt` 时跳过 PBR/boot-region 修复；**成功也不删** `<ESP>\EFI\ZJRESTORE\`（那是常驻模块，只有 Windows 侧「删除启动还原」才清）。
  - **体积代价**：内核 10.5MB + initramfs 38.7MB ≈ **51MB 常驻 ESP**（ESP 一般 ≥100MB，Windows 引导文件约 40MB，尚可；ESP 偏小需裁剪 initramfs）。**安全门禁**：GPT 目标仅 UEFI 固件放行（`IsUefiFirmware()`），BIOS+GPT 仍拒绝。
  - 🚧 **待真机/VM 回归**（QEMU 只验到"固件→Boot####→内核"；`bcfg boot -opt` 语法未攻下，故"OptionalData→命令行"是靠 UEFI 规范 + 内核源码 + Rufus/efibootmgr 先例，尚未实测）。✅ 2026-09-19（QEMU OVMF 验证固件直启内核）

### PIT-061

- PIT-061 **UEFI 救援"黑屏像卡死"：内核 `CONFIG_SYSFB_SIMPLEFB=y` 顶掉内建 efifb，而 `simpledrm` 是模块、没打包 → 没有 fbcon**（2026-09-19，用户实测踩到）：症状 = UEFI 还原时屏幕停在 `EFI stub: Loaded initrd from command line option` 加一个一闪一闪的光标，看着完全像死机；**其实内核在正常跑** —— 用户按 Ctrl+Alt+Del 热重启后，还原**已经做完并且进的是新系统**（进度条一直在输出，只是屏幕没有）。根因：Alpine 内核带 `CONFIG_SYSFB_SIMPLEFB=y`，开机把 UEFI GOP 帧缓冲注册成 "simple-framebuffer" 平台设备，**因此内建 `efifb`（`CONFIG_FB_EFI=y`）不绑定**；能接管它的只有 `simpledrm`（`CONFIG_DRM_SIMPLEDRM=m`），而 `build-alpine-initramfs.py` 的 `MODULE_EXCLUDES` 把整个 `kernel/drivers/gpu` 排除了（旧注释写"文字控制台用内建 efifb/vesafb"——**该假设在 UEFI 下不成立**）。**修复**：①构建脚本新增 `EXTRA_MODULES = ['kernel/drivers/gpu/drm/tiny/simpledrm.ko.gz']`（依赖闭包自动带回 `drm.ko`/`drm_kms_helper.ko`/`drm_shmem_helper.ko`，整包只 +0.13MB）；②`bootfiles/alpine/init` 里提前 `ldmod simpledrm`；③cmdline 仍是 `console=tty0`。**回归测试**：`tools/vmtest/uefi-screen.ps1`（QEMU+OVMF，`-display none` + monitor `screendump` 抓两次，字节差异 8.4% ⇒ 控制台活着；PASS）。**屏上文字只能 ASCII**：内核内置字体没有中文字形（中文显示成方框，同 PIT-036 GRUB4DOS 的教训），所以救援横幅一律英文、中文细节进日志。**BIOS 路径不受影响**（走 `vgacon`，`CONFIG_VGA_CONSOLE=y`）。✅ 2026-09-19（QEMU 截图实证 + 用户 VM 实证还原成功）

### PIT-062

- PIT-062 **Secure Boot 支持：走「微软签名的 shim → 我们签名的 UKI」链**（2026-09-19）：Secure Boot 开着时固件**只加载微软签名的镜像**，我们的内核未签名必被拒。调研同行做法：**易数一键还原** = 直接部署固件启动项 + **送微软 UEFI CA 2023 签名**；**傲梅一键还原** = 官方让用户进 BIOS 关 Secure Boot；**联想 OKR** = 同理需关。微软签名路线对个人**不可行**（EV 证书 ~$359/年可由 SSL.com 的 sole-proprietor 层级拿到、个人可申请，但 2025-10 起强制**年度第三方安全审计**，数千美元）。故我们选**免费且个人可用**的 shim+MOK（Ventoy/rEFInd 同款）。
  - **前提实测（关键）**：Alpine 内核 `CONFIG_LOCK_DOWN_KERNEL_FORCE_NONE=y`、**无** `LOCK_DOWN_IN_EFI_SECURE_BOOT`、`MODULE_SIG_FORCE` 未设 → **Secure Boot 开着也能加载未签名内核模块** ✓（否则救援层 1800 个 Alpine 模块全废，这条路直接走不通）。
  - **内核不吃 PE `.initrd` 节**：`efi_load_initrd()` 只认 `initrd=` 命令行或 `LINUX_EFI_INITRD_MEDIA_GUID` 设备路径 → 必须用 **systemd-stub 打 UKI**（它读自己的 `.cmdline/.initrd/.linux` 节并注册成设备路径）。
  - **链路**：`固件 → \EFI\ZJRESTORE\shimx64.efi（微软签名）→ grubx64.efi（= 我们签名的 UKI）→ systemd-stub → 我们的内核`。shim 找不到可信二级时**自动进 MokManager**（`mmx64.efi`）。
  - **一次性注册**：首次开机进蓝底 MokManager → `Enroll key` → 选随包的 `zj-mok.cer` → 重启。`IsMokEnrolled()` 比对 `MokListRT` 判断是否已注册；**未注册就暂存还原会在动目标分区之前拦住并提示**（避免"重启后什么都没发生"）。
  - **构建**：`tools/build-uki.py`（objcopy 打 UKI + osslsigncode 签名），`make package` 自动调用；产物 `dist/bootfiles/sb/{zjrestore-uki.efi,zj-mok.cer}`。⚠️ objcopy 的 `--change-section-vma` 必须 **ImageBase + 偏移**（预编译 stub 的 ImageBase=0x14df90000，照抄 systemd 文档的 0x20000 会报 `section below image base`）。
  - **资产**：`bootfiles/sb/{shimx64.efi,mmx64.efi,fbx64.efi,linuxx64.efi.stub}`（Ubuntu shim-signed 1.59 / Debian systemd-boot-efi 257）+ `keys/zj-mok.{key,crt,cer}`（**私钥不入库，务必备份**：丢了以后老客户已注册的 MOK 就对不上新签名）。
  - **安装**（`src/boot/uefi.cpp`）：`IsSecureBootEnabled()` → 部署 shim 布局、启动项指向 `shimx64.efi`；否则走原来的"启动项直启内核"（PIT-060）。GUI「安装启动还原」在 SB 机器上会提示需要重启一次注册。
  - `diag` 会打印 `firmware=` / `secureboot=` / 启动项状态。✅ 2026-09-19（构建链验证通过：UKI 49.3MB + 签名 `Signature verification: ok`；**待 SB 环境实测引导**）

### PIT-063

- PIT-063 **Secure Boot 两种走法对比：`shim`（标准）vs `bootapp`（零交互，借微软 bootmgr）**（2026-09-19）：真机 SB 测试时 `shim` 路线在 VMware 固件上只报 `Verification failed: (0x1A) Security Violation`、**没进 MokManager**（NTLite 的判据：只出错、无 MOK 界面 ⇒ 被拒的是 shim 链本身，常见于固件只信 Windows CA 或策略限制；Ventoy 文档也承认有这类固件）。为对比，加了第二条走法，用环境变量 `ZJ_SB_MODE` 切换（默认 `shim`）：
  - `shim`：固件启动项 → `shimx64.efi`（微软 CA 2011 签名）→ `grubx64.efi`(= 我们签名的 UKI)。**官方指定机制**，但首次要 MokManager 注册一次。
  - `bootapp`：`bcdedit /application bootapp` → `\EFI\ZJRESTORE\efiloader.efi`（[a1ive/efiloader](https://github.com/a1ive/efiloader)，**GPLv3，像 grldr 那样单独分发**，24KB，subsystem=16 正是 bootmgr 要求的格式）→ `loadoptions \EFI\ZJRESTORE\zjrestore-uki.efi` → 我们的 UKI。**零用户交互**，但要 `nointegritychecks true`（关掉 bootmgr 的完整性校验）+ `device boot`；单次启动用 BCD `bootsequence`（不走固件 `BootNext`）。代价：依赖 Windows/BCD 完好（ESP/BCD 坏了进不去），且 `nointegritychecks` 正是 BlackLotus(CVE-2023-24932) 在收紧的点。
  - 两条路都要**先有 UKI**（`tools/build-uki.py`）；`bootapp` 不需要 `zj-mok.cer`/MokManager。
  - ⚠️ 自签 MOK 证书**必须带 `extendedKeyUsage=codeSigning`**（`build-uki.py` 已加），否则 EFI 镜像签名永远验不过。
  - 🚧 待用户真机/VM 实测两条路并选定默认（`CurrentSbMode()` 里改一行即可）。

### PIT-064

- PIT-064 **就地还原：目标分区没被占用时直接写，不重启**（2026-09-19，用户规格）：原实现无论在哪运行都是"暂存 → 重启 → Linux 救援层 apply"，**在 PE 里、或还原到非系统盘时纯属多余**（用户实测吐槽："在 PE 中恢复镜像到 C 盘居然提示重启"）。现在 `StageRestore` 自动二选一：
  - **判据 `CanRestoreInPlace()`**（`src/app/ops.cpp`）：① 目标**不是正在运行的系统盘**（`GetWindowsDirectoryW()[0] != target.letter`）；② 能对 `\\.\X:` 加**独占锁** `FSCTL_LOCK_VOLUME`（卷上有打开的文件/句柄就会失败）。
  - **满足 → `RunDirectRestore()`**：`format.com X: /FS:NTFS /Q /Y`（不传 `/V` 以保留原卷标）→ `WimEngine::Apply`（**目录模式**，PIT-008）→ 仅当目标里出现 `Windows\System32\winload.exe`（=系统镜像）才跑 `bcdboot`（UEFI：`/s <ESP> /f UEFI`；BIOS：`/s X: /f BIOS`）→ **完成，不重启**。
  - **不满足 → 原路径**（暂存任务 + 重启 + Linux 救援，PIT-059）。
  - API：`StageRestore(req, err, bool* needReboot)`；GUI 据此决定是否 `RebootNow()`（就地完成时弹"还原完成，无需重启"），CLI 打印不同提示。
  - ⚠️ §2 禁令 1（重启类还原必须走 Linux）**不变**：就地还原不是"重启类"，两条路并存。
  - 🚧 待实测（PE 直装、还原非系统盘）。

### PIT-065

- PIT-065 **`nointegritychecks` 在 Secure Boot 开启时被策略保护 → bootapp 路线不可用**（2026-09-19，用户实测）：`bcdedit /set {GUID} nointegritychecks true` 在 Secure Boot 开启时报
  `设置元素数据时出错。该值受安全引导策略保护，无法进行修改或删除。`
  （= "The value is protected by Secure Boot policy and cannot be modified or deleted"）。⚠️ **这不是新补丁/BlackLotus 加固的产物**（初版 PIT 归因有误，已改正）：微软官方文档（`testsigning` 页）就把这条报错列为**预期行为**，并直接让用户"去 BIOS 关 Secure Boot"；早年（Vista/Win7 时代）的问答里报的是同一条错。**即：自 Secure Boot（Win8）引入以来，"关闭完整性校验"这类启动项就一直被策略保护**，与 Windows 版本/补丁新旧无关。
  - **结论**：Secure Boot 机器上"第三方工具零交互加载未签名代码"不存在可行路线（除非自费买 Microsoft UEFI CA 签名，见 PIT-062）。业界只剩：`shim + MOK`（一次性注册）或 `微软签名`。
  - `ZJ_SB_MODE=bootapp` 代码保留（宽松固件/未开 SB 时仍可用），但 `CurrentSbMode()` **默认 `shim`**。
  - 附带收益：这次测试同时验证了另外两个修复 ✓（ESP 空间：清理旧文件后 31.9MB → 81.3MB free；镜像说明截断：`%s`→`%ls` 后显示完整 `1 - Windows 10 专业版（Admin）`）。

### PIT-066

- PIT-066 **零注册候选：借 Canonical 的签名链（`shim` → Canonical 签名的 GRUB → 我们的内核）**（2026-09-19，待实测）：调研发现 GRUB 用普通 `linux`/`initrd` 命令加载时**可能不校验内核签名**（只有 `linuxefi`/`initrdefi` 校验；多条资料明写 "…boot … DESPITE the kernel is NOT SIGNED"，以及 "checks the kernel signature but **not the initrd**"）。而 Ubuntu 的 `grubx64.efi.signed` 与 `mmx64.efi` **是同一把证书签的**（实测均为 `CN=Canonical Ltd. Secure Boot Signing (2022 v1)` ← `Canonical Ltd. Master Certificate Authority`），而 Ubuntu 的 shim 内嵌的正是这把证书 → **shim 直接信任这个 GRUB，无需任何 MOK 注册** ✓。
  - 新增走法 `ZJ_SB_MODE=grub`：部署 `shimx64.efi + mmx64.efi + fbx64.efi + grub-ubuntu.efi(改名 grubx64.efi) + grub.cfg + 我们的内核/initramfs`，启动项 → `shimx64.efi`；`grub.cfg` 用 `linux`/`initrd` 指向 `/EFI/ZJRESTORE/vmlinuz-zjrestore` + initramfs。
  - **若成立**：零注册、零成本、无任何"绕过"（全部可执行文件都有合法签名）→ 成为 SB 机器上的默认走法。**若被 GRUB 拦住** → 转"变体 D"：用 Canonical 签名的 Ubuntu 内核 + 我们的 initramfs（initrd 从不受校验），代价是救援内核要换 + 体积涨。
  - 资产：`bootfiles/sb/grub-ubuntu.efi`（Ubuntu `grub-efi-amd64-signed` 1.215）、`bootfiles/sb/grub.cfg`。🚧 待 VM 实测（SB 开启）。

### PIT-066（二）零注册正解（现行）

- PIT-066 **零注册正解（已实测通过）：借 Canonical 的签名链 —— `shim` → Canonical 签名的 GRUB → **Canonical 签名的内核** + **我们的 initramfs****（2026-09-19）：Secure Boot 机器上"零交互 + 免费 + 不绕过"最终落在这条链上。调研与实测过程：
  - **变体 C（失败）**：`shim → Canonical GRUB → 我们的未签名内核`。QEMU/真机实测 GRUB 报 `error: bad shim lock signature` —— Ubuntu 的 GRUB **强制**校验内核（`shim_lock` 机制），未签名内核被拒（那条"普通 `linux` 命令不校验"的说法是过时的）。
  - **变体 D（成功）**：把内核换成 **Canonical 签名的 Ubuntu 内核**，initramfs 保持我们的（**UEFI 规则：initrd 从不校验** ✓）。Ubuntu 官方文档原文佐证：*"all pre-built binaries … **with the exception of the initrd image**, are signed by Canonical's UEFI certificate, which itself is implicitly trusted by being embedded in the shim loader"*、*"**Initrd images are not validated**"*。
  - **构建**：`tools/build-ubuntu-rescue.py` —— 复用 `build-alpine-initramfs.py` 的 Alpine 用户态组装（busybox/musl/wimlib/ntfs-3g **与内核无关**），只把内核+模块换成 Ubuntu 的：`linux-image-6.8.0-31-generic`（14.7MB，签名者 = `Canonical Ltd. Secure Boot Signing (2022 v1)`，**与 mmx64.efi/grubx64.efi 同一把证书**）+ `linux-modules-6.8.0-31-generic`（999 个 `.ko.zst`，**也带 Canonical 签名**）。产物仍是 `bootfiles/vmlinuz-zjrestore` + `initramfs-zjrestore.cpio.gz`（**文件名不变 → 部署逻辑零改动**）。
  - **两个坑**：① Ubuntu 的模块包**没有 `modules.dep`**（安装时才 depmod）→ 构建脚本从每个 `.ko` 内嵌的 `depends=` 字段自己生成；② **`modules.dep` 必须用二进制写**（Python 在 Windows 上文本模式会把 `\n` 变 `\r\n` → busybox 解析出带 `\r` 的路径 → `module X not found in modules.dep`，错误信息里路径尾巴那个点就是 `\r`）。
  - **QEMU 回归**：`tools/vmtest/uefi-ubuntu-smoke.ps1`（PASS：`Linux version 6.8.0-31-generic` → `SR: modules loaded` → `SR: block devs: … sda sda1`）。
  - **`ZJ_SB_MODE` 默认已改为 `grub`**（`shim`/`bootapp` 保留可选）。体积：内核 14.2MB + initramfs 32.5MB ≈ **47MB**（比 Alpine 版 49MB 还小）。
  - **安全性质**：没有绕过任何东西 —— 链上每个可执行文件都有合法签名，和 Ubuntu 正常开机完全一样；微软/Canonical 换证书时我们只换文件。
  - **后续**：① 服务器 RAID 模块（`vmd`/`megaraid`/老 `mpt*`/`isci`）在 Ubuntu 的 `linux-modules-extra`（113MB）里，需挑子集补入；② 内建模块的 `not found in modules.dep` 日志噪音可静音。
  - **验证**：✅ 2026-09-19 用户 VM（Secure Boot 开启）实测**通过**（零交互、无 0xc000007b、无 MokManager），`Secure boot mode: grub`。
  - ⚠️ **2026-09-23 已被取代**：为覆盖「只信 CA2023 的 2026 新固件」，整条链**换成 Debian**（shim 微软**双签** + Debian 签名 GRUB/内核），见 **PLAN §11.1**。本条目保留作历史记录；`ZJ_SB_MODE=grub` 的机制不变，只是资产换成 Debian 的。

### PIT-067

- PIT-067 **第二次部署引导层必失败：`menu.lst` 带 Hidden+System，`CreateFileW(CREATE_ALWAYS)` 被拒**（2026-09-20 用户在笔记本实测并修复）：症状见 `dist\logs\SysRecover-20260920.log` —— `安装引导层失败: deploy drive=C`，而 `copy OK ... grldr.mbr/grldr` 全成功、**唯独 `write menu.lst FAIL`** → 暂存中止、还原从未发生。根因：`WriteMenuLst` 用 `CreateFileW(..., CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, ...)` 打开**已存在且带 Hidden+System** 的 `menu.lst`（上次部署 `Hide()` 设的）→ `ERROR_ACCESS_DENIED=5`（与 PIT-037 的 `CopyOne` 同一个坑，当时这里漏了）。**修复**（`src/boot/grub.cpp` 的 `WriteTextFile`）：打开前先 `SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL)`。✅ 2026-09-20（用户修复；16:52 那次运行该错误已消失 → 证实生效）

### PIT-068

- PIT-068 **`bootfix\bootmgr` 第二次覆盖是同一个坑（静默失败 → `bootfix INCOMPLETE`）**（2026-09-20 笔记本日志暴露、AI 定位）：16:23 首次暂存 `bootfix: ok`，16:52 第二次变成 `bootfix: FAILED` + `bootfix INCOMPLETE` + `prepare bootfix failed` —— **但还原照样成功**，因为 `C:\ZJRESTORE\bootfix\` 里还留着**上次**写好的 BCD/bootmgr（"侥幸成功"；真机第一次跑就会真的缺补全包 → 目标机黑屏）。根因：`\bootmgr` 天生带 Hidden+System（`CopyFileW` 还会把源属性继承给目标），第二次 `CopyFileW(src, dst, FALSE)` 覆盖被拒；而那一行是 `ok &= CopyFileW(...)`，**没有失败日志**，只能看到上一层的 `INCOMPLETE`。**修复**（`src/boot/bootfix.cpp`）：新增 `ForceCopy()`（先归零目标属性 + 失败写 `copy FAIL <src> (err=N)`），`CopyTree` 内部与 `\bootmgr` 复制均改走它；并给 `CopyTree` 两处静默失败（源非目录 / `FindFirstFileW` 失败）补日志。✅ 2026-09-20（编译链接通过；**待下次还原验证**：期望 `bootfix prepared`）

### PIT-069

- PIT-069 **`software_dir` 缺失时救援层日志落不到软件目录**（2026-09-20 笔记本日志 `debug log NOT persisted (software dir not found)`）：`zjrestore-lite.sh` 只从任务 conf 读 `software_dir`；conf 找不到时回退读 `_zjresy*.log`（PIT-035），而**日志里只有 `software_path`（exe 全路径）、没有 `software_dir`** → `SD_REL` 为空 → `find_soft_dir` 直接 return 1。**修复**：`software_dir` 为空时从 `software_path` 去掉文件名当目录（`sed 's|\\[^\\]*$||'`）。✅ 2026-09-20（**待下次还原验证**：期望 `debug log saved: <software_dir>/logs/`）

### PIT-070

- PIT-070 **换机后重启蓝屏：BCD 恢复条目残留旧设备（`device=partition=D:`），而救援文件已在新布局的 C: → bootmgr `0xc000000F`**（2026-09-20 用户在笔记本实测并修复，= 用户报告的"第一次的问题"）：在笔记本上第一次暂存（16:23 `bootfix: ok`）后**重启即蓝屏**（Windows 引导错误 `0xc000000F`"所需设备无法访问"），救援层从未启动。根因：BCD 里的恢复条目 `{12345678-…}` **已经存在**（早前用旧布局把救援文件放数据盘 D: 时建的，条目里 `device=partition=D:`），而新布局（PIT-059）把 `grldr`/`grldr.mbr`/`menu.lst` 放在**目标盘 C:**；原 `BcdCreateBootsector` 遇到"条目已存在"就**跳过重设** → 文件在 C:、条目指 D: → bootmgr 去 D: 找 `\grldr.mbr` 找不到 → `0xc000000F`。**修复**（`src/boot/bcd.cpp`）：**条目已存在也不再跳过**，每次都把 `device partition=<本次部署盘>:` + `path \grldr.mbr` 重刷一遍（幂等 = 自愈），并顺带刷新 description 与 displayorder（先 `/remove` 再 `/addlast`，避免重复调用报错）。✅ 2026-09-20（用户修复，已由后续还原运行验证）

### PIT-071

- PIT-071 **任务执行中点关闭"完全没反应"：`Notify` 的 `m_busy` 早退把关闭点击吞成死代码，而唯一的取消通道只在析构函数里**（2026-09-20 用户实测踩到）：用户在朋友机器上测试备份、CPU 占用高想退出，**点右上角叉叉没有任何反应**，只能干等任务跑完。根因两处叠加：① `CMainForm::Notify` 开头就是 `if (m_busy) return;`，把 `CloseBtn` 的点击也一起吞掉 → 下面那段"忙时弹框拒绝"的分支**永远执行不到**（死代码）；② 真正能停任务的 `m_cancel = true` 全文件**只出现在析构函数**（`main_form.cpp` 头部）→ 关闭被拦 ⇒ 窗口不销毁 ⇒ 析构永不执行 ⇒ **取消信号永远设不上**。次要因素：worker 是默认线程优先级 + wimlib 吃满所有核 → 界面点击发飘。**修复**（用户规格：默认「继续等待」，可选「终止并退出」）：① `Notify` 把关闭按钮**提到 `m_busy` 早退之前**处理；② 新增 `AskBusyClose()`（`CConfirmDlg` 通用化为 `Ask2`：两按钮文案 + 默认项可指定；此处左=「继续等待」[默认，回车/ESC 均落安全项]、右=「终止并退出」）；③ 新增 `CancelAndExit()`：置取消位 → 保持消息泵地等 worker 收手（正常 <1s）→ `join()` → `CleanupIncompleteOutput()` 删除未写完的**临时文件** `<目标>.tmp`（**不是**最终路径：产物是原子写的，`wim.cpp::Capture` 先写 tmp、成功才改名 → 取消既不损坏旧同名镜像、也留不下半截成品）→ 关窗；10s 仍未停 → 兜底 `TerminateProcess` + `MoveFileEx(..., MOVEFILE_DELAY_UNTIL_REBOOT)` 登记开机删除；④ `OnTaskComplete` 见 `m_cancel` 只记日志、**不弹失败框**（否则会挡住关窗）；⑤ 两个 worker 入口 `SetThreadPriority(THREAD_PRIORITY_BELOW_NORMAL)`。✅ 2026-09-20（编译通过，版本 `0.1.4`；**待用户实测**：备份中点叉叉应立即弹框；选「终止并退出」应 1 秒内退出且半截镜像被删）

### PIT-072

- PIT-072 **PE 里明明"就地还原、不重启"，确认框却提示"重启后还原"：文案写死，与实际行为不符**（2026-09-21 用户实测）：用户在 **PE** 里还原，程序确实**不重启**就地开始，但确认框的按钮与正文仍是写死的「退出不重启」/「重启后还原」+「选择…将暂存任务并自动重启执行」→ 与实际行为矛盾、误导用户。**修复**：① 把 ops 内部的 `CanRestoreInPlace()` 提升为**对外可见**（`src/app/ops.h`），GUI 用它判断"这次要不要重启"—— 与 `StageRestore` 内部**同一个函数**，两处不会漂移；② `StartRestore` 分两套文案：需要重启 → `退出` / `退出并重启`（默认项）+「暂存任务，随后自动重启执行」；不需要重启 → `取消` / `开始还原`（默认项）+「目标分区当前未被占用，将立即就地还原，不需要重启」。✅ 2026-09-21（编译通过，`0.1.5`；**待用户实测**：PE 里应显示"不需要重启"那一套）

### PIT-073

- PIT-073 **`wimlib_get_xml_data` 返回的是 UTF-16LE（不是 UTF-8）；且镜像大小必须用 `TOTALBYTES − HARDLINKBYTES`**（2026-09-21 做"还原前空间预检 P1"时踩到并实测校准）：要读某个子镜像的**未压缩内容大小**，但我们打包的 `wimlib.h`（1.14）**没有** `wimlib_get_image_info()` ✗，只能走 `wimlib_get_xml_data()` 解析元数据 XML。两个坑：
  ① **编码**：返回的 XML 是 **UTF-16LE 带 BOM**（开头 `FF FE`，每个字符后跟 `00`），按窄串找标签会**静默失败**（返回找不到 → 预检被跳过、形同虚设）→ 必须先用 `WideCharToMultiByte` 转 UTF-8 再解析（代码按"第 2 字节是否为 0"判定）；另外 **XML 缓冲区要用 `free()` 释放** —— `wimlib_free()` 是释放 `WIMStruct` 的，**不是**通用释放器 ✗。
  ② **指标**：`<IMAGE>` 里的 `<TOTALBYTES>` 把**硬链接按独立文件计数**，拿它当"需要多少空间"会**高估近一倍** → 必须减掉 `<HARDLINKBYTES>`。
  **实测校准**：`TOTALBYTES 10.16GB − HARDLINKBYTES 5.09GB = 5.07GB`，与救援层 apply 日志的 `Extracting file data: 5188 MiB (=5.07GB)` **完全一致** ✓。
  **验证手法（可复用）**：用 **ctypes 直调 `dist/libwim-15.dll`**（`wimlib_global_init` / `wimlib_open_wim` / `wimlib_get_xml_data`）把 XML 打出来看 —— **只读，不碰任何分区**。✅ 2026-09-21（`0.1.6`）

### PIT-074

- PIT-074 **检测 UTF-16 不能只看 `buf[1] == 0`：带 BOM 时开头是 `FF FE`**（2026-09-21 做 P10 时踩到，**P1 也因此静默失效过**）：`wimlib_get_xml_data` 返回 UTF-16LE **带 BOM**（`FF FE`），而我们最初按"第 2 字节是否为 0"判断 UTF-16 ✗ → 带 BOM 时 `buf[1] == 0xFE ≠ 0` ✗ → 误判成 UTF-8 → 解析全空 → `images` 显示 `0.00 GB`、**空间预检静默跳过**（fail-open：不报错、看不出来 ✗）。**修复**：① 先识别 `FF FE` BOM；② 否则扫前 16 字节里有没有 `0x00`（UTF-16 文本特征）。**教训**：fail-open 的检查（解析失败就放过）**必须留一条日志**，否则坏了也发现不了 —— 这次是靠 `images` 打出 `0.00 GB` 才暴露的。✅ 2026-09-21（`0.1.7`）

### PIT-075

- PIT-075 **就地还原"第一次进度回调就中止"：进度回调返回值语义写反（`true` = 取消）→ `rc=76 WIMLIB_ERR_ABORTED_BY_PROGRESS`**（2026-09-23 用户实测踩到）：用户在 **PE（从光盘运行）** 和**把程序拷到 D 盘后**都遇到 `暂存失败：就地还原：应用镜像失败 rc=76 (The operation was aborted by the library user)`，误以为是只读介质或权限问题 ✗。真实原因：`ops.cpp::RunDirectRestore` 传给 `wim.Apply` 的进度回调写成 `return true;` —— 而 `wim.cpp` 的约定是 **返回 true = 请求 ABORT**（`(*c->fn)(pct,stage) ? WIMLIB_PROGRESS_STATUS_ABORT : CONTINUE`）→ **第一次回调就中止** ✗。**修复**：`return false;`（继续），并在该处写明语义。**为什么一直没发现**：就地还原此前**从未跑通**（`docs/07` 一直标"待实测"✓）；而"暂存 + 重启"路径的 apply 跑在 Linux 救援层里（不经过这个 C++ 回调 ✓），所以一直正常。**教训**：`bool` 型回调的"真=取消"这种约定极易写反 —— 注释里其实写了（`wim.h`），调用方没看；日后这类语义建议直接叫 `onProgressReturningAbort()` 之类，或改用枚举。✅ 2026-09-23

### PIT-076

- PIT-076 **手动输入镜像路径时按钮不变蓝（GUI 只监听"浏览…"/拖入，不监听输入框变化）**（2026-09-23 用户实测）：备份模式在"浏览保存位置"右侧输入框里**手打**路径/文件名 → "开始备份系统"仍是灰的 ✗，必须点一次"浏览…"才变蓝。原因：`CMainForm::Notify` 只处理 CLICK / SELECTCHANGED / ITEMSELECT，**没有监听输入框文本变化** → `m_wimPath` 不同步、`UpdateMainAction()` 不触发。**修复**：在主窗口 `HandleMessage` 加 `WM_COMMAND` + `EN_CHANGE`（`CSkinEditUI` 内部是原生 EDIT，其变化通知送到父窗口 = 主窗口）→ 同步 `m_wimPath` + 刷新按钮；还原模式下若路径指向存在的文件，顺手解析子镜像（用 `m_lastLoadedWim` 防抖，避免每敲一键就重开 WIM）。✅ 2026-09-23

### PIT-077

- PIT-077 **手动输入/粘贴的路径"鼠标一移开就消失"、按钮一直灰：原生 EDIT 子窗口与控件文本从不同步**（2026-09-23 用户实测）：在"系统→文件"页往"浏览保存位置"输入框**手动打字或粘贴**路径/文件名 → **鼠标一移开字符就没了** ✗，"开始备份系统"始终灰 ✗，必须点一次"浏览…"才能用（用户原话："别人复制粘贴一段路径和名字过来却无法使用"）。根因：`CSkinEditUI` 内部是**原生 EDIT 子窗口**（用户输入进的是它），而 `DoPaint` 在**无焦点时改用控件自绘**（`if (!IsFocused())`），自绘的是**控件自己的 `GetText()`** ✗ —— 两者从未同步 → 失焦后画出空的 ✓；按钮读的也是控件文本 ✓。"浏览"能用纯属巧合：它调 `SetText()` 写的正是**控件** ✓。**修复**：① `CSkinEditUI::SyncTextFromNative()`（`GetWindowTextW` 回读子窗口 → 不同则 `SetText`），在 `DoEvent` 里每次兜一次；② GUI 的 `EN_CHANGE` 处理器**直接读原生子窗口**文本（双保险：EN_CHANGE 时控件文本可能还没同步）。**教训（流程）**：本轮有两次"构建失败但照样提交"（`EnableEnvPrivilege` 拼错、`ui_skin.cpp` 漏 `<string>`）—— **提交前必须单独跑一次构建并确认通过，不要把构建和提交写进同一条命令**（失败也会继续往下走）。✅ 2026-09-23（`0.1.20`）

### PIT-078

- PIT-078 **两处 GUI 显示/文案回归**（2026-09-23 用户实测，都在"就地还原"流程里）：① **跑的过程中进度百分比一直空着** ✗（只有完成时才显示 100% ✓）—— 根因：`SetProgress()` 只设置进度条（`CRoundProgressUI::SetValue`），**从不写 `PercentText`**；那个标签只在 `OnTaskComplete` 里写 → 中途为空，且上一轮的 `100%` 会**残留**（用户看到进度条旁一个像被压扁的 `%）` 的怪字符）。**修复**：`SetProgress()` 一并更新 `PercentText`（`>0` 写 `NN%`，`==0` 清空以抹掉残留）。② 就地还原完成提示写的是"**系统还原已完成，无需重启**" ✗ —— 用户会理解成"现在就能用还原好的系统"（实际必须**重启**才进得去 ✗）。**修复**：改为"**系统还原已完成，用时 …。重启后即可进入恢复的系统。**"。✅ 2026-09-23（`0.1.23`）

### PIT-079

- PIT-079 **救援层模块裁剪：白名单里 `kernel/fs/` 是"整目录" → 把网络/集群/嵌入式文件系统全拉了进来；加 EXCLUDE + 键盘补收后 779→494 模块、initramfs 36.3→20.5MB（dist 54.2→38.5MB）**（2026-09-24 调研 + 实施）：
  · **调研方法（可复用）**：解出包内全部 4225 个 `.ko.xz` 的 `depends=` 建依赖图 → 模拟"白名单 + 闭包" → 对候选删除集做**反向依赖校验**（保留下来的模块是否依赖被删模块）。这一步是"不破坏兼容性"的唯一依据。
  · **关键事实**：① `fuse`、`atkbd`/`i8042`/`libps2`/`serio`/`input-core`、`fb_efi`/`fb_simple`/`framebuffer_console`/`vt` 在 Debian 内核里都是**内建**（`modules.builtin` + `/boot/config-*` 可证）→ 不需要对应模块，PS/2 键盘本来就可用；② **PIT-061 的"UEFI 黑屏"是 Alpine 内核特有的**（它 `SYSFB_SIMPLEFB=y` 顶掉内建 efifb），Debian 的 `FB_EFI=y` 直接接管 → **整条 DRM/KMS 链（含 `gpu/drm/tiny` 的 bochs/cirrus）可删**；③ `lpfc`/`qla2xxx`（在 PLAN 的"关键 31 项"里）的 `depends=` 含 `nvme-fc`/`nvmet-fc` → 必须**连带保留** `nvme-fc/nvme-fabrics/nvmet-fc/nvmet/configfs/nvme-auth/nvme-keyring`，否则破坏 31/31；④ iSCSI/FCoE offload 卡（bnx2fc/bnx2i/cxgb3i/cxgb4i/qedf/qedi/qla4xxx）会经闭包拉进 `drivers/net`+`infiniband`+`target`+`libfc` 一大串 → 删卡即删整串。
  · **实施**：`build-debian-rescue.py` 新增 `EXCLUDE_PREFIXES`（对白名单**和**闭包**同时**生效）+ `EXTRA_KEEP`（USB HID：`hid`/`hid-generic`/`usbhid`，让救援 `#` shell 能用 USB 键盘）+ **构建期断言**：① 被排除却被保留模块需要 → 直接 `raise`；② 保留模块的依赖缺失 → `raise`。fail-fast，杜绝"静默产出缺依赖的坏包"。
  · **结果**：779 → **494** 模块；33.48 → 14.10 MB（`.ko.gz`）；initramfs 36.32 → **20.47 MB**；dist 54.17 → **≈38.5 MB**。（2026-10-06 起模块改存 Debian 原生 `.ko.xz`、initramfs 15.2MB，见 PIT-125）
  · **有意保留**（判断项，用户 2026-09-24 拍板）：`xfs/btrfs/bcachefs/f2fs`（镜像放这些分区时用得到）、`ext4`、FC HBA（维持 31/31）。
  · **附带发现（未处理）**：`kernel/drivers/ufs/` 从来不在白名单 → `ufshcd-core` 一直没进救援层（x86 Windows 上罕见，暂不加；init 里 `ldmod ufshcd-core` 一直静默降级）。
  · ✅ 2026-09-24（构建断言通过 + 产物自检：`hid`/`usbhid`/`hid-generic` 与全部关键存储/fs 模块 present，`nfs`/`cifs`/`ocfs2`/`kvm`/`ib_core`/`drm` 等 absent；**待 QEMU/VM 回归**）

### PIT-080

- PIT-080 **裁剪后的完整回归：BIOS 端到端还原演练跑通；顺带修好三个已失效的测试资产**（2026-09-24）：PIT-079 删模块后，除"启动能起来"外还须证明"**真能还原**"。逐项验证：① 构建期断言 ✓；② `modules.dep` 一致性（494 行、0 悬空、0 缺失；`modules.builtin`/`modules.order` 在）✓；③ 产物自检（关键模块 present、被删项 absent）✓；④ UEFI SB 链（`uefi-ubuntu-smoke`）✓；⑤ UEFI 控制台（`uefi-screen`，见下）✓；⑥ UEFI 非 SB 直启（`uefi-smoke`）✓；⑦ 固件 `Boot####` 直启（`uefi-bootentry-smoke`）✓；⑧ BIOS/GRUB4DOS（新 `bios-smoke.ps1`）✓；⑨ **端到端还原演练**（`mk-drill.py` + `run-drill.ps1`）：`apply done` → `bootfix installed` → `boot region OK (0x54=fa33c08e, sector1 non-empty)` → `RESTORE DONE: /dev/sda2` → `reboot: Restarting system` ✓（证明 `mkntfs`/`wimlib apply`/PBR 所需的模块都在）。
  · **修的三个测试资产**：① `uefi-screen.ps1` 原用"两次截图字节是否不同"判断控制台 —— 裁剪后救援启动更快、30s 时已停在静态 `#` → **误报 FAIL**；改为**确定性串口断言**（`efifb: probing` + `Console: switching to colour frame buffer device` + `fb0: EFI VGA frame buffer device`），顺带证明**删掉 DRM/KMS 后 UEFI 控制台仍正常**（`CONFIG_FB_EFI/FB_SIMPLE=y` 内建接管，efifb 绑定）。② `mk-drill.sh`/`mk-testdisk.sh` 都引用了**已删除的 `bootfiles/restore.sh`**（`set -e` 直接中止）→ 删掉该行；`mk-drill.sh` 改为转调新写的 `mk-drill.py`。③ **`mformat` 多分区坑**：`mformat -i img@@off` 用**文件大小**决定卷大小 → 对多分区镜像会把整盘当成一个卷，三个分区 FAT 相互覆盖（症状：sda2/sda3 变 `non DOS media`、sda1 的 `grldr/menu.lst` 凭空消失）→ **正解：每个分区单独建精确大小的镜像 → mformat/mcopy → 再拼回磁盘**（`mk-drill.py`）。另：旧脚本注释的 sda3 偏移 `822704128` 是**错的**（正确 = `1606848*512 = 822706176`），排查时被它误导过一轮。
  · 新增 `tools/vmtest/bios-smoke.ps1`（自建 512MB FAT32 活动盘 + QEMU `-boot c`，确定性串口断言）。✅ 2026-09-24（全部 PASS；真机/VMware 实测仍待用户）

### PIT-081

- PIT-081 **单元测试（`make check`）第一次运行就抓到真 bug：`sysinfo` 的 `shortTag` 用 `swprintf("%s")` → 默认文件名变成 `Win10.1`（PIT-007 重演）**（2026-09-24）：实现 PLAN §12 第 3 项的**零依赖单元测试**（`tests/`：自写微框架 `tiny_test.h` + `make check`，不引 gtest）。为可测，把**纯逻辑从 Windows 调用里抽出来**：`ComposeSystemDescription(RawSysInfo)`（sysinfo）、`DefaultExclusionConfig/BuildExclusionContent/CloudFolderNames`（exclude）、`BuildTaskConf/BuildTaskJson/BuildRestoreLogText`（task 契约文本）、`Crc32`（zip）。**首次运行 4 个用例失败**：`shortTag` 得 `Win10.1` 而非 `Win10.19044` —— 根因 `swprintf(tag, 32, L"Win%d.%s", family, build.c_str())`，MinGW 下 `%s` 当**窄**串（PIT-007）→ `build` 只剩首字符。**影响**：备份模式默认文件名（`<日期>Win10.<build>备份.esd`）一直是错的（`Win10.1`）。修 `%ls`。**附带收益**：`exclude` 测试里加了 **PIT-056 回归守卫**（默认排除清单绝不能含 `.LOG1/.LOG2/regtrans-ms/NTUSER.DAT/TM.blf`）。✅ 2026-09-24（17 C++ 用例 / 57 断言 + 13 Python 断言全绿；`0.3.1`）

### PIT-082

- PIT-082 **启动器未嵌 `requireAdministrator` → 在 UAC 开启的机器上 `CreateProcessW` 真程序必失败（740）**（2026-09-24 用户 VM 实测）：新的发布形态「x86 启动器 + `x86/` + `x64/`」上线后，**Win7 x86（UAC 关）正常**，但 **Win10 x64 双击启动器报「找不到或无法启动 …\x64\SysRecoverUI.exe」**（而 `x64/` 下文件其实都在）。根因：真程序清单是 **`requireAdministrator`**，而非提权的启动器用 **`CreateProcessW`** 启动它 → **`ERROR_ELEVATION_REQUIRED(740)`**（`CreateProcess` **不会**自动提权，只有 `ShellExecute … runas` 才会）。**修复**：给**两个启动器也嵌入 `requireAdministrator` 清单**（复用既有清单资源对象 `SysRecover_rc.o` / `SysRecoverUI_rc.o`，与真程序一致 → 双击时由加载器弹一次 UAC，之后 `CreateProcess` 子程序因父进程已提权而成功）；并在启动器错误框里带上 `GetLastError()` 错误码（740/2/126/193 一眼可判）。✅ 2026-09-24（`0.3.4`；**待用户 Win10 x64 复测**）

### PIT-083

- PIT-083 **外部审查（"问题清单"）逐条核实与修复：3 项属实已修、1 项不成立、1 项低价值不改；修复中自己又踩一坑（已修）**（2026-09-26）：外部 AI 给了 13 条清单（2 高/4 中/7 低），**逐条核实后**处理如下。
  · **属实并修复**：
    ① **H-02 / M-01（真实失效组合）**：`hv_vmbus/hv_storvsc/pci-hyperv/xen-blkfront` **在镜像里但不在 `init` 主名单**；且旧兜底只在"**零分区**"时触发，而 `usb-storage/uas` 在主名单 → **插着 U 盘 `list_parts` 就非空 → 兜底永不触发** → Hyper-V/Xen 系统盘不可见时直接失败。**修复**：4 个模块加进主名单；兜底条件改为"**找不到任务日志**"（把日志扫描抽成 `scan_for_log()`，扫不到就全量 modprobe 再扫一遍）。
    ② **H-01 / M-02（静默失败）**：`ldmod` 把"内建"与"真缺失"都报成 `builtin or not bundled (ok)` → **UFS 驱动缺失被静默**。**修复**：失败时查 `modules.builtin`，区分 `builtin (ok)` / `WARN not bundled` / `not applicable (no such device)`。顺带删掉 `simpledrm/nvme-common/t10-pi/linear`（既非模块也非内建；详见下），`multipath` 改为真实模块名 `dm-multipath`。
    ③ **M-04**：PE 下 `CanRestoreInPlace()` 直接放行、**完全不试卷锁**。**修复**：PE 下也试锁，失败只 `LogWarn` 继续（不拒绝）。
    ④ **L 系列**：`history.jsonl` JSON 转义不全（补 `\n\t\r\b\f` 与 `\u00xx`）、`grub.cpp::CopyOne` 固定 512 栈缓冲（改按长度分配）、`build-debian-rescue.py` 的 `gpu/drm/tiny` 白名单与 EXCLUDE 重复（删白名单）、`ls|head -1` 改循环、`init` 重复注释。
  · **不成立（不改）**：**M-03** "MBR offset 兜底可能匹配错盘" —— `_disk` 名字解析错（如 `nvme0n1p`）时 `dd` 会失败 → `_off=0` → **安全地匹配不上并退出**，不会写错盘。仍**顺手加固**（先验扇区 0 的 `55aa`、用 sysfs 父目录取整盘名），几乎零成本且更正确。
  · **低价值/有风险（不改）**：**L-07** BitLocker 检测只认中/英关键词（非中英系统可能漏判）—— 目标用户是中文 Windows；改成语言无关要引 WMI/未公开 API，风险 > 收益。
  · **⚠️ 修复中自己踩的坑（已修）**：把 HV/Xen 加进主名单后，**普通机器上 `modprobe hv_vmbus` 报 `No such device`**，被新 FAIL 逻辑当成失败 → **每台普通机 8 行 FAIL 噪音**。补 `*"No such device"*` 分支降级为 `not applicable (no such device)`（硬件不存在属正常）。
  · 回归：`make check` ✓、`bios-smoke` ✓（日志无 FAIL；UFS 显示 WARN）、**BIOS 端到端演练 ✓**（`apply done`→`boot region OK`→`RESTORE DONE`）。✅ 2026-09-26（`0.3.11`）

### PIT-084

- PIT-084 **借鉴 DreamGrain 电子教室的工程实践**（2026-09-26）：读了同总项目下 `DreamGrainClass/AGENTS.md`（Veyon 定制），只挑"**机器可校验 / 能防错**"的几条落地：
  · **崩溃处理（我们原本完全没有）**：新增 `src/common/crash.{h,cpp}` —— `SetUnhandledExceptionFilter` 捕获未处理异常 → `<exeDir>\logs\crash\` 写 **`MiniDumpWriteDump`**（**动态加载 `dbghelp.dll`，零链接依赖**）+ **可读文本**（异常码/地址/访问违例方向+地址/**调用栈地址+所属模块+偏移**/命令行/版本）+ `last.txt`；CLI/GUI 入口各装一次；`diag --zip` 会把 `logs\` 一起打包。回归 **`make crash-test`**（故意触发访问违例；实测 `.dmp` 41KB + `.txt` 均落盘）。
  · **"指针"一致性校验**：新增 `tools/check-docs.py`（接进 `make check`）—— 校验 `version.h` 的 SemVer、`SYSRECOVER_CONTRACT_VERSION` ↔ `AGENTS`/`docs` 的 `contract_version`、**救援脚本 `get_task` 读的键 ⊆ `task.cpp`（BuildTaskConf+BuildRestoreLogText）写的键**、`dist/version.json` ↔ `version.h`。当前 22 键写 / 8 键读，PASS。
  · **AGENTS 新增 §17**：开工序、编译/打包纪律（**只有 `dist/` 是交付物**）、**品牌与术语约束表**（九转/SysRecover/`zj*` 技术标识/「讨论」链接/维护者 —— 防改名漏网）、**AI 工具资源纪律**（并发搜索 2~4 封顶、能 Read 就不搜、少跑 PS/短命令）、**删除暂存区 `docs\老旧文档暂存\`**（只进不出）、崩溃处理说明。
  · **启动计时改环境变量**：`SYSRECOVER_STARTUP_TIMING=1`（原为编译期宏 `ZJ_LOG_STARTUP`，要重编）—— 学 DreamGrain 的 `DREAMGRAIN_STARTUP_TIMING`。
  · **没抄**：他们的教师端/学生端双端分包、上游 rebase 台账、CMake/Qt 工具链（与本项目无关）。
  · 回归：`make check` ✓（含 check-docs）、`make crash-test` ✓、`make package` ✓。✅ 2026-09-26（`0.3.12`）

---

### PIT-085

- PIT-085 **第二轮外部评审（多角色专家组）收拢 7 项并落地**（2026-09-26，`0.3.13`）：8 个角色（安装部署/运维/测试/安全/兼容/UX/代码/文档）独立评审后交叉印证，收拢成 7 项按顺序实施（清单见 PLAN §13.4）：
  · **P1 网络镜像防呆**：还原**系统盘**要重启进救援层，而救援层**没有网络** → 镜像在 UNC/映射盘上必然失败（且可能已格式化目标）→ `src/app/ops.cpp::IsNetworkPath()`（`\\` 开头或 `DRIVE_REMOTE`）在**暂存前**拒绝并给明确提示（"先复制到本地分区"）。**实测**：`\\localhost\D$\...\test.wim` → 走到 `restore mode: staged reboot` 后 `staged restore rejected: image on network path`，**未写任务/未动 BCD** ✓。
  · **P2 契约版本握手**（设计里早有、实现一直没有）：`task.cpp` 的 `BuildTaskConf`/`BuildRestoreLogText` 补写 `contract_version=%d`；`zjrestore-lite.sh` 用 `ZJ_CONTRACT=1` 核对 `get_task contract_version`（**缺键视为 1**，兼容旧版）→ 不匹配 `say ERROR` + `exit 1`。**实测两条**：① 演练 conf 无该键 → `ZJ: contract_version=1 (ok)` 且还原照常；② 把演练 conf 的 `contract_version` 写成 **2**（模拟旧版任务）→ `ERROR: contract_version mismatch: task=2 rescue=1`，**`apply`/`mkntfs` 均未发生**（fail-fast 保护目标分区）✓。
  · **P3 CLI 自描述**：`<命令> --help` / `help <命令>` / 无参打印总览 + 退出码含义（`src/cli/main.cpp`）。
  · **P4 `dist/README.txt`**：面向使用者的说明（UTF-8 **带 BOM** + CRLF，双击记事本可读），`make package` 拷进 dist（文件名 ASCII，避免 Makefile 命令行中文坑）。
  · **P5 日志轮转**：`src/common/logger.cpp::PruneLogs()`（`SysRecover-*.log` 保留 14 天、`logs/crash/crash-*` 保留最近 30 个），`LogInit` 末尾调用。**实测**：60/30 天前的假日志被清、近期保留 ✓。
  · **P6 图标**：**美术图由产品维护者提供**（`resources/icon-source.psd` 原稿 + `resources/icon-source.ico` 256×256，粉金环形"九转"螺旋）；`tools/make-icon.py` 只负责**降采样成标准多尺寸**（256/128/64/48/32/24/16 → `resources/SysRecover.ico`）——**单尺寸 ICO 会被 GDI 粗暴缩放，任务栏/资源管理器小尺寸发糊**（无源图时脚本回退到内置造型：品牌蓝圆角方块 + 白色环形箭头）。**仅 `src/gui/SysRecoverUI.rc` 加 `1 ICON "SysRecover.ico"`**（与 `1 24` 清单**同 id 不同类型，不冲突**）；**`src/cli/SysRecover.rc` 不带图标（2026-09-26 用户裁定：dist 根目录两个 exe，只有 GUI 入口保留品牌图标，避免"哪个是入口"歧义；复检 CLI 应无 RT_GROUP_ICON）**，Makefile GUI 的 windres 加 `-I resources`；`main_win.cpp` 建窗后 `WM_SETICON`(ICON_BIG/SMALL)。**实测**：从 GUI exe 提取到 32×32 该图案 ✓。⚠️ 16px 偏糊（3D 光泽画通病）——如需更清晰可另做 ≤24px 的简化造型。
  · **P7 `make smoke`**：顺序跑 `bios-smoke`/`uefi-smoke`/`uefi-ubuntu-smoke`（**不进 `make check`**，耗时数分钟）。
  · **顺带修**：CLI 退出码映射 `return rc==4?4:(rc==5?5:1)` → `(rc==2||rc==4||rc==5)?rc:1`，让"参数/用法错"回到 §9 承诺的 **2**。
  · **⚠️ 本轮自己踩的坑（已修，教训记此）**：改完 `bootfiles/zjrestore-lite.sh` 后**忘了按 AGENTS §17.2 重建 initramfs** → 演练跑的是**旧脚本**，新加的握手行一直"看不见"，一度怀疑逻辑没执行 ✗。**规则重申：改救援层脚本 ⇒ 必须 `python tools/build-debian-rescue.py`，再回归**（本次重建后 494 模块/20.5MB 不变 ✓，握手行立即出现 ✓）。另：`tools/vmtest/parse-initramfs.py` 的用法是 `<archive> <mode> [target]`，用错参数会**静默无输出**（别误判成"内容不在镜像里"）。
  · 回归：`make check` ✓（17 用例/57 断言 + 13 Python + check-docs PASS）、`make crash-test` ✓（dump OK）、`make package` ✓（x64+x86）、BIOS 端到端演练 ✓（`apply done` → `boot region OK` → `RESTORE DONE`）。✅ 2026-09-26（`0.3.13`）

### PIT-086

- PIT-086 **坏盘健康检测落地；NVMe 协议数据必须放 `query->AdditionalParameters`（偏移 8），否则 `ERROR_INVALID_PARAMETER(87)`**（2026-09-26，`0.3.15`）：用户把「坏盘/文件系统损坏检测」从"讨论后再做"划出（与代码签名/GUI 提取一并裁定，见 PLAN §13.4）——还原要**格式化目标分区**，若目标盘已现坏道，还原完系统照样起不来，必须**先警告再动手**。
  · **实现**：`disk.cpp::QueryDiskHealth()` 双路 —— ① **ATA/SATA**：`IOCTL_ATA_PASS_THROUGH` 发 `SMART READ DATA(0xD0)` 取属性 5/197/198 + `SMART RETURN STATUS(0xDA)` 判"驱动器自报即将故障"；② **NVMe**（Win10+）：`IOCTL_STORAGE_QUERY_PROPERTY` + `StorageDeviceProtocolSpecificProperty`（=50）+ `ProtocolTypeNvme`/`NVMeDataTypeLogPage`/`RequestValue=0x02` 读 SMART/Health 日志页（Critical Warning / Media Errors / Percentage Used / Temperature）。判定：`caution = ATA 自报故障 || 待定>0 || 无法纠正>0 || 重映射>100 || criticalWarning!=0 || mediaErrors>0`。
  · ⚠️ **坑（本次核心）**：**协议数据必须从 `query->AdditionalParameters`（= 偏移 8）开始**（MSDN "Working with NVMe Drives" 原文："The start of the STORAGE_PROTOCOL_SPECIFIC_DATA is the AdditionalParameters field"）。最初按 `sizeof(STORAGE_PROPERTY_QUERY)=12` 顺排、又试 9 —— **三种布局全被驱动以 `ERROR_INVALID_PARAMETER(87)` 拒绝**，而普通属性（`StorageAdapterProperty`/`StorageDeviceProperty` plain 查询）**都正常** → 一度怀疑"MinGW 把枚举值定错了"，**其实 50 是对的**（SDK：`StorageDeviceIoCapabilityProperty=48`，其后 adapter=49 / device=50；MinGW 头与 SDK 逐字一致），**错的是偏移**。输出侧同理：数据地址 = `&descriptor->ProtocolSpecificData + ProtocolDataOffset`（不是"描述符尾"硬算），MSDN 的校验是 `offset < sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA)` 即拒。
  · ⚠️ **排查手法（可复用）**：① 先用 plain 属性验证 ioctl/句柄本身可用，再逐个换布局打印错误码（一次性探针，别在产品代码里试错）；② **直跑 `g++.exe` 必须先把 `mingw64\bin` 加进 PATH** —— 否则 `cc1plus` 报 `0xC0000135`（缺 DLL），**exit=1 且 stderr 为空**，极易误判成"源码语法错"（`mingw32-make` 路径下不会遇到，因为 make 会带 PATH）。
  · **展示三处**：GUI `AskDiskHealthWarning`（`CConfirmDlg::Ask2`，**默认"取消"=安全项**；静默模式跳过、只记日志）、CLI `restore` 暂存前打印、`diag` 每盘一行（`smart=ata|nvme|unavailable` + 关键指标）。取不到 SMART（USB 桥 / RAID / Win7 无 NVMe 属性）→ `smartKnown=false` **fail-open 放行**（与 BitLocker 同款"只警告不硬拦"，见 PIT-083③）。
  · **实测**（本机）：`disk0` SATA HDD → `smart=ata realloc=0 pending=0 uncorrect=0`；`disk1` NVMe → `smart=nvme crit=0x00 media=0 errlog=0 used=1% temp=46C`（数值合理）。⚠️ **未实测"真坏盘告警"**（本机盘都健康）——需拿有坏道的盘回归一次"弹框/打印确实出现"。
  · **附带（同批提交）**：产品中文名改 **「九转还原」**（原"九转一键还原"，用户 2026-09-26："可读性和分辨性更强"）—— 全仓 22 处 + 2 处"产品定义行"同步（皮肤 XML、窗口/对话框标题、README、docs、AGENTS §15/§17.3 品牌表），复检零残留；`九转还原` 四字标题更居中好看。✅ 2026-09-26（`0.3.15`）

### PIT-087

- PIT-087 **热备份前置 VSS 检查（用户 2026-09-26 规格）：服务不可用时 wimlib 只报 `rc=89`，只字不提服务名**（2026-09-27 实测定位并落地）：备份靠 `WIMLIB_ADD_FLAG_SNAPSHOT` → Windows VSS 卷影快照。**实测三事实**（Win11 dev 机）：① `vss=禁用` 或 `swprv=禁用`（任一）→ 备份均失败 `备份失败(rc=89): Unable to create a filesystem snapshot`，**报错完全不提服务** → 用户无从下手（"为什么没有成功"的根源）；② Windows 默认「停止+手动」态 wimlib 自己能拉起，但**跑完不关**（vss+swprv 留 Running 残留，与"完成后关闭"规格不符）；③ `wimlib.h` 明文 VSS 快照不支持 WoW64（64 位系统必须 64 位程序——正常由 selfarch 自举保证，x64 缺失时兜底提示）。**实现**：新增 `src/common/vss.{h,cpp}`（`vss::BackupGuard`，纯 Win32 SCM API `OpenSCManagerW/OpenServiceW/StartServiceW/ControlService/QueryServiceStatusEx`，禁 WMI）；`RunBackup` 算出 `snapshot` 后（RegFlush 之前）调 `Ensure()`：VSS + swprv 逐个查——**缺服务/被禁用/启动失败/30s 超时 → err = 多行中文处理指引**（services.msc 与 `sc config X start= demand && sc start X` 两条路，附 rc=89 因果说明），阻断在动数据之前；「停止」则启动并记录原状态。**析构回滚**（覆盖所有退出路径，含失败路径）：只停「亲眼确认过备份前是停止、且现在在运行」的（=只关我们开的；备份前就在跑 → 不动）。**坑**：`ErrorAdvice` 的 `has("管理员")` 会命中提示词里的"管理员窗口" → 追加误导性建议"以管理员身份运行"（失败根本不是权限）→ 在 `ErrorAdvice` **最前**加 VSS 短路分支返回 `{}`。**回归 4 场景全过**：正常（停→启→备份成功→停回）/ vss 禁用（指引+状态原样+exit=1+无 wim）/ swprv 禁用（指引+**已拉起的 VSS 回滚停回**）/ 预运行（成功+保持运行不关）。`make check`/`make package` 绿。✅ 2026-09-27（`0.3.17`）

### PIT-088

- PIT-088 **i18n 落地三坑（2026-09-27，全部已修 + 单测/冒烟覆盖）**：
  · ① **`--lang` 开关"完全没生效"**：`Utf8Args()`（`src/cli/main.cpp:207`）**已经丢掉 argv[0]**，而我写解析循环时按惯例 `for (i = 1; ...)` 起跳 → 正好跳过位于**首位**的 `--lang` → `langArg` 恒空、参数也没被吃掉（**日志 `cmd: --lang en help` 原样回显就是判据**：参数若被吃掉应只显示 `cmd: help`）。改 `for (i = 0; ...)`。**教训**：加命令行开关前先确认 `Utf8Args()` 的口径，别照搬"argv[0] 是程序名"的肌肉记忆。
  · ② **`.lang` 的键含 `=` 被从中间截断**：格式是 `key=value`，`备份失败(rc=%d): ` / `就地还原：应用镜像失败 rc=` / `打开目标卷失败 err=` 等 **9 个键本身含 `=`** → `line.find('=')` 命中键内部 → 键被截成半截、值带着剩下的键（症状：门禁同时报"缺键"9 个 + "死键"9 个 + printf 占位符不匹配，极像译文表写错，**其实格式本身有缺陷**）。**修复**：`lang_escape` 把 `=` 转义成 `\=`，分隔符改为**首个未转义的 `=`**，`i18n.cpp::Unescape/LoadTextLocked`、`wrap::lang_escape`、`check-i18n.py::split_entry` **三端同口径**；单测 `i18n_escaping_and_equals_split` 直接用 `备份失败(rc=%d): ` 当回归用例。⚠️ 只把 `=` 转义而不改分割逻辑是**无效**的（`\=` 里还是 `=`，照样被切）。
  · ③ **门禁 `load_lang` 用了 `strip()`** → 把键的**前导/尾随空格**吃掉（`' 备份'`→`'备份'`、`'    已用 '`→`'已用 '`）→ 报"重复键"假阳性。`.lang` 的键值**可以带空格**，必须与 C++ 解析器一致：只去尾部 `\r`、空行/首字符 `#` 跳过，**绝不 strip**。
  · **冒烟口径（可复用）**：CLI —— `dist\x64\SysRecover.exe help`（应中文）/ `--lang en help` / `set SYSRECOVER_LANG=en && ... help`（后两者应英文），看首行 `SysRecover · Command Line` vs `SysRecover 九转还原 · 命令行`；GUI —— 起 `SysRecoverUI.exe` 读 `Process.MainWindowTitle`：zh=`九转还原` / en=`SysRecover`（读之前 `[Console]::OutputEncoding = UTF8`，否则控制台按 GBK 打出来像乱码）。✅ 2026-09-27（`make check` 24 用例/148 断言 + `make package` 绿，双语冒烟通过）

### PIT-089

- PIT-089 **Duilib 的悬浮提示（tooltip）在我们配置下**永远不显示**，根因是 `cbSize` 传了 `sizeof(TOOLINFO)`：MinGW 的 `TTTOOLINFOW` 带一个 v6 才有的 `void* lpReserved`（x64 下 `sizeof`=**72**），而本 exe **无 comctl32 v6 清单**（PIT-018）→ 跑 **v5**，它只认 `TTTOOLINFOW_V1_SIZE`(56)/`V2_SIZE`(64) → `TTM_ADDTOOL` 返回 **FALSE**、`TTM_GETTOOLCOUNT`=0 → 窗口建了但没工具、永不显示（2026-09-27/28 定位）**：
  · **症状极具误导性**：`WM_MOUSEHOVER` 到达（`msg=673`）、控件 `GetToolTip()` 非空（`tip=32`）、`CreateWindowEx` 成功（`GetTooltipWindow()` 有句柄）—— 三层都"正常"，但 tooltip 窗口 `IsWindowVisible=0`、`rect=(0,0)-(0,0)`、`toolcount=0`，屏幕无气泡。极易误判为"光标没停够久""DPI 不对""消息被 UIPI 拦"。
  · **两层叠加（都要修）**：① `cbSize`（v5 拒绝 72）；② Duilib 的 `m_ToolTip.uFlags = TTF_IDISHWND` **没有 `TTF_SUBCLASS`**，且全库**从不发 `TTM_RELAYEVENT`** → 即使注册成功也收不到鼠标消息（MSDN：不设 `TTF_SUBCLASS` 就必须自己 relay）。
  · **修复（选"自管 tooltip"，不动 third_party）**：`CMainForm` 里自己 `CreateWindowEx(TOOLTIPS_CLASS,…)` + `cbSize=TTTOOLINFO_V1_SIZE` + `uFlags=0`（矩形工具）+ `uId=1`；`WM_MOUSEMOVE` 时把命中控件的 `GetPos()`/`GetToolTip()` 写进 `TOOLINFO`、`TTM_SETTOOLINFO`，再 `TTM_RELAYEVENT`（**relay 必须在更新之后**，否则命中测试用的是旧矩形）；移到无提示区域时清空 `rect` + `TTM_POP`；`WM_MOUSELEAVE` 时 `TTM_POP`；析构里 `DestroyWindow`。
  · **诊断手法（可复用，一步到位）**：`TTM_ADDTOOL` 后立刻 `SendMessage(hTip, TTM_GETTOOLCOUNT)` 打日志 —— `added=0 / toolcount=0` 就是没注册上，别再去调光标停留时间/DPI。`TTM_GETTOOLCOUNT = WM_USER+13`（A/W 同号），`sizeof` 用 `std::to_string(sizeof(TOOLINFO))` 一起打，72 vs 56 一眼看穿。
  · ⚠️ 想直接改 Duilib 也行（`m_ToolTip.cbSize = TTTOOLINFO_V1_SIZE;` + `uFlags |= TTF_SUBCLASS`），但那样得改 `third_party` 且 `TTM_TRACKPOSITION` 在没 `TTF_TRACK` 时是空操作（定位不可控）；**自管更可控**。
  · **冒烟口径（脚本已入库：`tools/ui/tooltip-smoke.ps1`，`powershell -File tools\ui\tooltip-smoke.ps1`，退出码 0/1）**：DPI-aware 探针把光标**先移开再落到**控件上（同点 `SetCursorPos` 不产生 `WM_MOUSEMOVE` → 不触发 hover），等 ~1.6s，枚举 `tooltips_class32` 里 pid 命中且 `toolcount>0 && IsWindowVisible` 的窗口，抓其矩形；再移到空白处断言它消失。中英文各 3 个控件（`RepairBootBtn`/`Silent`/`BootMenuBtn`）全过。✅ 2026-09-28（`make check` 24 用例/148 断言 + `make package` 绿，`0.4.1`）

---

### PIT-090

- PIT-090 **UEFI 引导写入的三个"静默失败"全部堵上（bcdboot rc / 空间 / 产物）+ 找不到 ESP 的回退 + `repair-boot` 一键修复**（2026-09-29，`0.4.2` 批次；分析全过程见 `docs/15`）：背景是机房学生机"还原后只剩『选择操作系统』空菜单"（判定为**镜像侧**，已停止深挖）与"DiskGenius 重建分区后找不到引导盘"（**我们代码的锅**）。逐条核实后确认 **5 处真缺陷**并修复：
  · **① bcdboot 返回码被丢弃**（`ops.cpp` 暂存 UEFI / 就地 UEFI / 就地 BIOS 三处只 `LogInfo`）→ 现在 `rc != 0` 即 `fail-closed` 并把 BFSVC 原文给用户（实测拿假 Windows 目录触发：`rc=193 尝试复制启动文件失败`）。
  · **② ESP 空间**：`InstallUefiBootEntry` 余量只有 **1MB**，且**先**拷我们 33MB 载荷、**后**跑 bcdboot → 空间被自己挤爆（实测满盘时 bcdboot `rc=112` 无声失败）→ 余量提到 **16MB**（bcdboot 实测需 ~8.6MB），并把**暂存流程改成"先 bcdboot、后放载荷"**。
  · **③ 没有产物断言**：新增 `boot/bcd_parse.{h,cpp}`（纯逻辑 `BcdEnumShowsOsEntry`）+ `uefi.cpp::VerifyEspBcd` —— 断言 BCD 里 `displayorder` 非空且有 `winload` 条目，且 `bootmgfw.efi`/`Resources\bootres.dll`/`Fonts\*_boot.ttf`/`zh-CN|en-US` 齐全（bcdboot "写一半"的特征）。**这是无论根因都能拦住"空菜单"的保险丝。**
  · **④ 找不到 ESP 只认精确类型 GUID**：新增 `disk.cpp::FindEspPartitionFallback`（**FAT 分区且根下有 `\EFI\Microsoft\Boot\bootmgfw.efi`** —— 固件只认路径不认类型 GUID）＋ `DescribePartitions()`（纯 ASCII 分区表摘要），报错文案改成能指导"漏建 ESP / 被标成 Basic Data"。
  · **⑤ 没有修复手段**：新增 CLI `SysRecover.exe repair-boot [--disk N --part M]`（找系统盘 → 找 ESP（含回退）→ 模板/空间预检 → bcdboot → 产物校验），**已坏的机器不必重装**。
  · 顺手：`extract --dest X:\`（**盘符根**）此前必报"无法创建输出目录"（`CreateDirectoryW("X:\\")` 失败）→ 现在把根目录视为已存在；失败时 `progress.json` 不再一律写 `percent=100`，改写**最后一个已知进度**。
  · 验证：`make check` 28 用例/178 断言 + `check-i18n`（**322** keys）全绿；`--lang en help repair-boot` 正确；`repair-boot` 负例带分区表摘要、`rc=1` 且**零写盘**；`extract --dest W:\` 实测成功。
  · ⚠️ **待做**：UEFI 分支的**机器侧回归**（本机 BIOS+MBR，走不到 ESP 分支）。✅ 2026-09-29

### PIT-091

- PIT-091 **`make package`（双架构）必须把两个工具链的 `bin` 都放进 PATH，否则 x86 那半**静默**失败**（2026-09-29，本机重装 i686 工具链时踩到）：只把 `D:\Prog\ProgIDE\mingw64\bin` 加进 PATH 就跑去 `make package`，x86 侧第一条编译就挂：
  `D:/Prog/ProgIDE/mingw32/bin/i686-w64-mingw32-g++ … -c src/disk/disk.cpp -o build-x86/…` → `mingw32-make: *** [build-x86/app/disk/disk.o] Error 1`，**编译器一行诊断都不打**（`cc1plus` 找不到自己的 DLL，stderr 空）→ 极易误判成"源码有错"。实测把 `D:\Prog\ProgIDE\mingw32\bin` 也加进 PATH 后，同一条命令立刻成功、`make package` 双架构通过（PIT-086 记的是"直跑 `g++.exe`"的同款坑，这里是 **make 场景**）。
  · **i686 工具链来源**（本机 2026-09-29 装回）：mingw-builds（niXman）与 x64 **同源同版本** —— `i686-14.2.0-release-posix-dwarf-ucrt-rt_v12-rev0.7z`（88MB，`https://github.com/niXman/mingw-builds-binaries/releases/tag/14.2.0-rt_v12-rev0`），解压出的顶层目录就是 `mingw32/`，直接解到 `D:\Prog\ProgIDE\` 即可（本机 x64 也是 mingw-builds：`x86_64-posix-seh-rev0` + UCRT `rt_v12-rev0`）。
  · 验收：`i686-w64-mingw32-g++.exe --version` → `i686-posix-dwarf-rev0 … 14.2.0`；`objdump -f dist\SysRecover.exe` → `pei-i386`、`dist\x64\…` → `pei-x86-64`。✅ 2026-09-29

---

### PIT-092

- PIT-092 **UEFI 固件 + MBR 盘（CSM/Legacy 装的 Windows）被误判成 UEFI 引导 → "未找到 ESP 分区"（0.4.1 起一直存在，2026-09-29 才修）**：用户在 **Win10 x64 + MBR 分区**上还原，直接失败并把截图存成 `C:\2222.png`：**"暂存失败 / UEFI 机器未找到 ESP 分区，无法部署引导层"**。
  · **根因**：`ops.cpp` 的引导链判据用的是 `disk.cpp::IsUefiFirmware()`（= `GetFirmwareType()`，只看**平台**是不是 UEFI，**不看当前是怎么启动的**）。"UEFI 平台 + legacy 引导的 MBR 系统"很常见（OEM 预装、克隆盘、GPT→MBR 转换、在 Legacy 模式下装的系统），这类机器**根本没有 ESP**，于是：① 暂存：`FindEspPartition` 失败 → abort（0.4.3 后文案变成带分区表摘要的"未找到可用的 ESP 引导分区"，但**依然失败**）；② 就地还原：同样走错链。
  · **修法**：**按"目标磁盘分区风格"选引导链**，而不是固件类型 —— 新增 `src/boot/bootpath.{h,cpp}`（纯逻辑 `ShouldUseUefiBoot(firmwareIsUefi, diskStyle)`：**GPT→UEFI/ESP、MBR→BIOS/GRUB4DOS**、风格未知→退回固件类型）+ `ops.cpp::UseUefiBootFor(target)`；暂存/就地/`repair-boot` 三处判据、以及"单次启动"（`BootNext` vs `bcdsequence`）全部改用同一判据，并新增日志 `boot path: UEFI/ESP | BIOS/GRUB4DOS (firmware=…, target disk style=…)` 便于现场判断。
  · **回归**：单测 `boot_path_choice`（UEFI+MBR / BIOS+MBR / GPT×2 / Unknown×2），`make check` **29 用例 / 184 断言**。✅ 2026-09-29
  · ⚠️ **待做**：真机（UEFI 固件 + MBR 盘）跑一次暂存→重启，确认日志是 `boot path: BIOS/GRUB4DOS` 且能进救援层。

### PIT-093

- PIT-093 **「安装菜单」原来只装"恢复环境"、不绑镜像 —— 用户以为装了却没内容（2026-09-29 用户反馈并定义新语义）**：用户在本机（MBR）与家里台式机（UEFI）都试过：**没选镜像时点「安装菜单」也提示安装成功**，于是问"都没有镜像，这是安装了个什么菜单？"。核实：`main_form.cpp::ToggleBootMenu` 只做"部署救援环境（内核+initramfs；BIOS 另加 `grldr`/`grldr.mbr`/`menu.lst`）+ 常驻启动项"，**完全不涉及镜像与目标分区**；而救援层找不到任务文件时直接 `no log and no conf, abort` → 落到 `#` shell（PIT-035/PIT-042）。即：单独点它装出来的入口"进去也没活干"，只有"暂存+重启"那条路（`StageRestore` 写契约 + 单次启动）才真正还原。
  · **新语义（用户 2026-09-29 规格）**：菜单项 = **一键还原"当前选中的镜像 → 第二步选中的目标分区"**。
    - 未选镜像（或镜像不可用）/ 备份模式 → 按钮**灰色**；已安装 → 文字变「删除菜单」；**换镜像 = 先删除再安装**（悬停提示写明当前绑定的镜像→分区）。
    - 点「安装菜单」弹确认框（3 行文案，明确"选该菜单项会格式化该分区"），确认后写入常驻契约 + 常驻启动项（**不设单次启动、不重启**，留给用户在开机菜单里自己选）。
  · **实现**：`ops.cpp` 把 `StageRestore` 重构为 `StageRestoreImpl(..., bool menuEntry)`，新增 `StageRestoreMenu`（共用全部安全检查/契约写入/引导层部署，差别只在"是否设单次启动"）；`task.cpp` 新增纯逻辑 `TaskConfGet()` + `ops.cpp::ReadMenuBinding()`（GUI 判断"绑了哪个镜像/是否换过"）；GUI `RefreshBootMenuBtn()` 改三态、`UpdateMainAction()` 末尾统一刷新（选镜像/选分区/刷新按钮一处不漏）。
  · **坑**：`CConfirmDlg` 正文**只显示 3 行、单行标签超宽会静默裁切**（PIT-039）→ 确认文案必须压成 3 行短句（第一版 7 行被裁了一半）；回车默认项要与高亮按钮一致（右=动作=回车，ESC=取消）。
  · **回归**：单测 `task_conf_get`（含"值不去空格/前缀键不误命中/注释跳过"）；`make check` **30 用例 / 195 断言**；GUI 截图实测四态：未选镜像=灰、选镜像后=亮、已安装=「删除菜单」+悬停说明、确认框 3 行不裁。✅ 2026-09-29

### PIT-094

- PIT-094 **常驻菜单装好了却"按什么键都选不了"：`{bootmgr} timeout=0` 时 bootmgr 不停留直接进 Windows**（2026-09-29 用户实测）：用户在笔记本（Win10 x64 MBR）与本机都装了「安装菜单」，重启后**无论按什么都进不去菜单**；手动 `bcdedit /set {bootmgr} timeout 30` 后成功进菜单、选中并**完整还原系统**（= PIT-093 的端到端真机验证通过 ✓）。
  · **timeout=0 的来源**：凡我们还原过的机器，目标 BCD 由 bootfix 生成且**故意写 timeout 0**（`bootfix.cpp`："启动菜单不等待"——单条目时无感知，一旦加上常驻恢复条目就不停留）；新装机器本身也可能就是 0。
  · **修复（用户 2026-09-29 规格：1 秒；"原来大于 1 就保留原值"）**：`StageRestoreMenu` 的 BCD 分支（BIOS + Secure Boot 的 bootapp 模式）装菜单时**只升不降**：原值 ≥1 一律保留、0/缺失补成 **1 秒**（1 秒足够用上下键选、几乎不拖慢开机）；设置失败 **fail-closed 中止**（菜单装上但选不了 = 静默坏，正是本次的坑）。**纯 UEFI 不碰** —— 其常驻项在固件 `BootOrder`（**开机按 F12** 选），与 bootmgr timeout 无关。
  · **实现**：`bcd.h::ParseBootmgrTimeout()`（纯逻辑、单测覆盖；**不依赖标签语言**——按"整行恰好两 token、第二 token 全数字且 ≤1 天"取值，兼容本地化/其它写法）+ `bcd.cpp::BcdGetBootmgrTimeout/BcdSetBootmgrTimeout`；`ops.cpp` 菜单分支调用；GUI 成功提示与悬停提示按 `UseUefiBootFor()`（已提到 `ops.h`，与 `StageRestoreImpl` 内部同一函数）分链措辞：BIOS=开机菜单选、UEFI=按 F12。
  · **顺带**：BCD 恢复条目描述由 `Tr(L"一键还原恢复环境")` 改为 **ASCII `SysRecover Recovery Environment`** —— bootmgr 菜单里中文描述在部分机器上显示成方框（PIT-054 实测），且与 UEFI bootapp 条目（desc `SysRecover`）、GUI 提示「选 SysRecover」三处一致（旧译文键随 skeleton 自动清除）。
  · **边界（有意不改）**：还原完成后目标 BCD 是 bootfix 的单条目 + timeout 0 → 菜单自动消失、开机不停留，无需回写；**删除菜单也不回写 timeout**（单条目不显示菜单、无感知；双系统用户自己的停留时间不被动）。已手动调到 30 的机器按"只升不降"继续 30，想改回 1 秒手跑 `bcdedit /set {bootmgr} timeout 1` 即可。
  · **回归**：单测 `bcd_parse_bootmgr_timeout`（英文输出/本地化标签/CR/无行尾/非数字/超限值跳过）→ `make check` **31 用例 / 204 断言**、`check-i18n` **328 keys**、`check-widths` OK（新 EN 状态文案曾超 638px 宽度门禁，已缩短）；`make package` 双架构。**待复测**：新构建在真机装菜单应自动把 0 → 1（日志 `bootmgr timeout: 0 -> 1s`），开机能用上下键选。✅ 2026-09-29

### PIT-095

- PIT-095 **VMware EFI 下按 F12 后"黑屏 + 光标数分钟"不是死机：菜单逻辑已就绪、仅画面渲染极慢**（2026-09-29 VM 实测 + 用户定位，`0.5.0` 结案）：测试机 `初心Win10x64`（Workstation 17.6.1，UEFI+SecureBoot+NVMe）按 F12 后停在黑屏左上一个光标（截图 `1111.png`），**什么都不按要几分钟后才画出 Boot Manager 菜单**（`2222.png`：`Boot normally / Windows Boot Manager / EFI NVME / EFI SATA / EFI Network / SysRecover / Enter setup / …`，**无倒计时行**），极易误判死机（本会话最初即如此误判，自动化探测绕了很久）。
  · **真相（用户破案）**：黑屏光标阶段菜单**输入已在线** —— `回车` = 默认 `Boot normally` = 秒进原系统；**盲按 ↓ 导航到 `SysRecover` 回车 = 秒进还原**（用户实测光标态 6 次↓；完全画出的菜单里 `SysRecover` 是第 5 次↓ —— 光标态起点/项数与画出后略有出入，以实测为准）。慢的只有**渲染**，构建与输入都不慢。
  · **日志铁证**：`vmware.log` `2026-09-29T14:49:40 About to do EFI boot: SysRecover` → 固件成功加载条目 → 恢复全流程正常跑完（`mkntfs` + apply + 重启进新系统，用户确认"可以正常恢复"）。即恢复链路（F12 菜单列出条目 / 固件加载 / 救援层执行）全部验证 ✓。
  · **定性**：VMware Workstation EFI 固件的**菜单绘制延迟**（固件侧；UEFI 变量没有"菜单默认项/菜单超时"开关，我们改不了），**与产品无关** —— 真机（PIT-093/094）无此现象；理论上菜单构建只读 NVRAM 变量（毫秒级）。**可选未做**：摘 `Boot0003` 的 A/B 对照定量定责（需关机离线改 NVRAM + 人工按 F12）。
  · **规避（记录在案，日后排查不再当死机）**：F12 后不要等画面 —— 直接 ↓×N + 回车选 `SysRecover`；正常恢复走暂存 `BootNext` 主流程，根本不按 F12。✅ 2026-09-29

### PIT-096

- PIT-096 **「镜像搜索」选第二项必闪退：自定义消息 `WM_PROGRESS_UPDATE = WM_APP+1` 与 Duilib 的内部异步通知泵撞车**（2026-09-30 用户两次实测崩溃，`0.5.1` 修复）：用户实测"搜索找到文件 → 点输入框下拉 → 选中第 2 个 → 程序立即退出"，崩溃报告 `0xC0000005 read address 0x8`，两次（`F:\...\logs` 与 `dist\logs`）**RVA 逐字节相同**（+0xB1E7 / +0xFDB2 / +0x77C44）。
  · **定位手法（可复用）**：崩溃转储 txt 有 RVA → `addr2line -a -f`（**PE 必须给完整 VMA = ImageBase 0x140000000 + RVA，给 RVA 只会输出 `??`**）+ `objdump -d --start-address=<VMA>` 反汇编 —— 一眼看出 `HandleMessage` 在 `call OnProgressUpdate` 之后，`OnProgressUpdate` 开头 `mov 0x8(%rdi),%rsi` 且 `%rdi`=lParam=**0**（`std::wstring` 的 size 字段 = +8 → "read 0x8"）。
  · **根因**：`src/gui/main_form.h` 曾把 `WM_PROGRESS_UPDATE` 定义为 `WM_APP+1`，而 **Duilib `CPaintManagerUI::PostAsyncNotify()`（UIManager.cpp:2870）也 `PostMessage(WM_APP+1, 0, 0)`**（供 `MessageHandler` 的 `case WM_APP+1` 消费，调用链在 `WindowImplBase::HandleMessage` 我们基类调用**更深处**）。我们先截获该消息并把 lParam=0 当 `wstring*` 解引用 → 空指针。触发源：下拉框第二次重建（`LoadWimImages` → `RemoveAll` 旧条目 → Duilib 延迟清理 `AddDelayedCleanup` → `PostAsyncNotify`）—— **正好解释"第一次填入（空框）不崩、一选中就崩"**。副作用：撞车期 Duilib 的异步通知**从未被泵过**（`m_bAsyncNotifyPosted` 卡 true）。
  · **修复（双保险）**：① 消息重编号 `WM_APP+10/11/12`（Duilib 仅用 +1；代码注释写明禁回 +1）；② `HandleMessage` 的进度分支加 `if (lParam)` 防御 —— 再有撞车/异常投递只 `LogWarn` 丢弃，**永不解引用**。
  · **教训**：自定义 `WM_APP+N` 前必须 grep 第三方 UI 库的 `WM_APP` 用法；另外本崩溃再次证明 `PIT-084` 崩溃报告的价值 —— 有 RVA + 反汇编就能离线定罪，不需要复现时挂调试器。✅ 2026-09-30

### PIT-097

- PIT-097 **点输入框弹下拉后"文字跟着鼠标拖选、必须再点一下才恢复"：`EN_SETFOCUS` 里直接 `TrackPopupMenu` 吃掉了这次点击的 `WM_LBUTTONUP`，原生 EDIT 永远停在"左键按住"状态**（2026-09-30 用户实测反馈，`0.5.2` 修复）：选完镜像后，镜像名从鼠标位置开始处于选中态，鼠标左右拖动会继续扩大选区，像"左键没松开"（实际已松开）。
  · **机制**：原生 EDIT 在 `WM_LBUTTONDOWN` 处理链里 `SetFocus` → 同步发 `EN_SETFOCUS` 给父窗口 → 我们在此直接开模态菜单 → 菜单立即捕获鼠标，随后那次真实的 `WM_LBUTTONUP` 被菜单循环吞掉 → EDIT 内部 `fMouseDown` 永不复位（且菜单关闭后 capture 已归零，不会自动补），之后在文本上移动/拖动鼠标都被当成拖选，直到下一次完整的点击（DOWN+UP）才恢复。症状与"菜单窗口自己卡住"无关，**全部状态都在 EDIT 控件内部**。
  · **修复**：`EN_SETFOCUS` 只 `PostMessage(WM_OPEN_PICK_MENU)`（`WM_APP+13`，避让 Duilib 的 +1，PIT-096），处理时**先等左键抬起**（`GetAsyncKeyState(VK_LBUTTON)`，未抬起则 15ms 后重投），确认这次点击已完整结束再 `ShowImagePickMenu()`；并复查焦点仍在输入框（用户已点别处则不弹）。副作用排查：`m_pickGuard` 仍保留（菜单关闭后 `SetFocus` 的防重入），Tab 触发（无鼠标）时左键未按 → 立即弹出，与原行为一致。
  · **排查手法**：用户描述即典型"控件错过 UP"特征 —— 选区锚点=鼠标点击点、拖动即扩选、再点一下恢复；凡"模态菜单/对话框从鼠标事件处理链里同步弹出"都要过这一遍（同类：文件对话框从 EN_SETFOCUS 弹也会有此问题，本项目文件对话框走按钮点击不受影响）。✅ 2026-09-30

### PIT-098

- PIT-098 **备份 100% 后到完成弹窗之间是「静默期」：收官阶段只写 progress.json、不回调用户可见进度，GUI 状态栏冻在上一阶段**（2026-10-01 用户实测反馈）：非静默模式的完成弹窗本来就存在（`OnTaskComplete`），但用户在 100% 后等「很久」才弹出、状态栏一直停在「写入 100%」（只有"已用"时钟在走），怀疑是勾选了 ESP 备份。核实：① 主镜像 capture 到 100% 后**永远**还要跑 `Probe`（`wimlib_open_wim(CHECK_INTEGRITY)` = 全文件哈希扫描，**无回调**，大镜像几十秒），`--verify` 时另有 `engine.Verify`（同样无回调）—— 这才是静默期大头；② ESP 并入本身**有**进度（`Append` 传了 `progress`），但阶段名是 wimlib 的 `write`，看不出在做 ESP；③ `ProgressUpdate()` 只写 `progress.json`（外部工具看得到、用户看不到）；④ GUI 进度节流 `elapsed<100ms && pct<100` 会把 100% 后 100ms 内刚发出的阶段标记也吞掉。**修复**：① `ops.cpp::RunBackup` 收官各阶段补 `progress()` 回调（esp / verify / probe，含两处失败路径的 Probe）；② `WimEngine::Verify` 增加 `ProgressFn` 参数并注册回调，`ProgressThunk` 接 `WIMLIB_PROGRESS_MSG_VERIFY_STREAMS`（真实字节进度 + 速度/ETA）—— CLI `--verify` 与 `verify` 命令不再全程静默（`CmdVerify` 先手动 `ConsoleProgress(0,"verify")` 亮阶段名：open 扫描期注册不了回调）；③ `StageCn` 新增 esp/verify/probe → 中文（备份ESP / 校验镜像 / 完整性检查，前缀匹配兼容速度后缀）；④ 节流放行"阶段名变了"的首次上报（新成员 `m_lastStageRaw`，备份/还原两 worker 同款）。**顺手修**：`CmdBackup` 没设 `g_phase` → capture 期间 `ConsoleProgress` 把 `progress.json` 的 Phase 覆盖成 `idle`（补 `g_phase="backup"`）。✅ 2026-10-01（`make check` 31 用例/208 断言 + check-i18n **344 keys** + `make package` 双架构；`0.6.4`；GUI 100% 后状态栏依次显示 备份ESP→（校验镜像）→完整性检查 **待用户实测**）

### PIT-099

- PIT-099 **救援层日志永远停在第一次快照：`find_soft_dir` 的缓存被命令替换丢进子 shell，后续 `persist_log` 全部失败**（2026-10-02 客户日志取证定位，`0.6.8`）：客户报"Linux 还原后 explorer 卡"，但发回的两份 `zjrestore-debug.log` 都停在 `found image`、没有任何 apply 记录 —— 一度无法判断卡在哪一步。读代码发现：`persist_log` 用 `_m=$(find_soft_dir)` 取挂载点，而命令替换跑在**子 shell**，函数里写的 `SOFT_MNT` 缓存赋值就地丢失；可函数成功时**已经把软件分区挂在 `/tmp/zj_soft` 且不卸载** → 之后每次 `persist_log` 重扫分区，重新挂载同一分区到同一挂载点必然 busy 失败 → 返回 "software dir not found"。于是**只有第一次快照落盘**（正好停在 `found image`），apply/失败/成功全都写不进去。**修复**：`find_soft_dir` 改为在**当前 shell** 直接设置全局 `SOFT_MNT` 并 `return 0`（调用方不再用 `$(...)`），缓存真正生效；并对失效挂载加 `[ -d ... ]` 复核。**同批"诊断底座"**（0.6.8）：① 新增**日志实时镜像** `start/stop_log_mirror`（后台每 2s 把完整 log + `apply.out` + `apply_io.log` 镜像到软件目录，EXIT trap/重启前停掉）；② 目标准备与扫描全阶段 `timeout`（blkid 30s / dd 30s / mkntfs 300s / mount·ntfsfix 60s），超时落 `dmesg`；③ 修 `mode=` 骗人 bug：dir 回退时置 `APPLY_MODE=dir` + `APPLY_FALLBACK=1`，汇总行如实打印 `mode=… fallback=…`，并写 `WARN: DEGRADED RESTORE` + `zjrestore-DEGRADED.txt`；④ 失败路径统一 `say_dmesg`；⑤ apply 失败打印 wimlib 输出**尾部**（`[ERROR]` 在末尾）+ `rc_name` 解码（46=NTFS_3G / 59=SET_SECURITY / 72=WRITE…），回退日志记录实际挂载类型（ntfs3/fuse）；⑥ 新增**内核 cmdline 实验钩子**（`zjapply=block|dir|dirfuse`、`zjfs=ntfs3|fuse|auto`、`zjtarget=format|keep`，见 `bootfiles/alpine/init`）+ `build-debian-rescue.py` 的 `ZJ_RESCUE_APPLY_MODE/ZJ_RESCUE_OUT`（把默认模式注入**打包副本**、输出到不同文件名）——用于"强迫降级"印证实验，**不影响默认流程/产线包**；已产出 `tools/vmtest/base/initramfs-dir-ntfs3.cpio.gz`（一级）与 `initramfs-dir-fuse.cpio.gz`（二级），QEMU 演练两种模式全链 PASS（日志 `mode=dir fs=ntfs3|fuse`、`apply rc=0 fallback=0`、`RESTORE DONE`，软件目录落盘完整）。**回归**：`mk-drill.py` 现在在演练盘 sda3 预置 `ZJRESTORE/` 软件目录，QEMU 演练后可从盘上读出**完整** `ZJRESTORE/logs/zjrestore-debug.log`（含 `log mirror: on`、`apply rc=0 elapsed=… mode=block fallback=0`、`RESTORE DONE`）+ `zjrestore-apply.out`（wimlib 原文）；BIOS 端到端演练全链 PASS。✅ 2026-10-02（`make check` 31 用例/208 断言 + check-docs/i18n/widths 全过；`0.6.8`）

### PIT-100

- PIT-100 **SMART「即将故障」误报：`SmartReturnStatus` 读错寄存器（查了 [2]/[3]，应为 [3]/[4]）**（2026-10-02 客户实测反馈，`0.6.9`）：客户笔记本上「换了两块盘（含旧 SSD）都提示 SMART 阈值已超」，但第三方工具正常、只显示盘「清过零」。核实代码：ATA8-ACS 规定 SMART RETURN STATUS 的结果在 **Cylinder Low/High（`CurrentTaskFile[3]/[4]`）** —— 健康 = `0x4F/0xC2`、预测故障 = `0xF4/0x2C`；旧实现查的是 `[2]/[3]`（SectorNumber/CylinderLow）。在「控制器**不回填** CurrentTaskFile」的机器上，`[2]/[3]` 仍是请求时写的 `0x4F/0xC2` → **健康盘被判"阈值已超"**（部分控制器会回填/清零，所以本机 PIT-086 测试一直没暴露；本机现在实测 `st=unk srcl=0x00 srch=0x00`）。**修复**：按 `[3]`/`[4]` 判 OK/over，回读值既非 `0x4F/0xC2` 也非 `0xF4/0x2C` → 不下结论（`failingKnown=false`，fail-open）；`DiskHealth` 增加 `failingKnown/srCl/srCh`，`diag` 输出 `st=ok|over|unk` + 原始寄存器（现场一眼可证）。✅ 2026-10-02

### PIT-101

- PIT-101 **备份 100% 后静默十几分钟才弹完成框：`Probe` 用 `open(CHECK_INTEGRITY)` = 全文件扫描且注册不了进度回调**（2026-10-02 客户反馈收拢 + PIT-098 收尾，`0.6.10`）：客户 44G 备份「显示百分百后等 12 分 14 秒才弹出完成」。根因：写出后必做的 `WimEngine::Probe` 走 `wimlib_open_wim(WIMLIB_OPEN_FLAG_CHECK_INTEGRITY)` —— 打开阶段还没拿到 `WIMStruct`、注册不了回调，GUI/CLI 只能停在上一阶段。**修复**：`Probe` 改两步 —— ① 普通 `open`（快）查 `write_in_progress`/`image_count`（保留 PIT-057 的"半截镜像"友好报错）；② `wimlib_verify_wim` 做全文件校验，**带 `VERIFY_STREAMS` 真实字节进度**；`RunBackup` 里 `--verify` 与默认后置检查**合并成一次**（以前两者都跑 = 双倍全扫）。还原暂存前的 Probe 同样受益（「校验镜像」现在有百分比）。CLI 实测：写出后 `verify 0%→35%→100%`。✅ 2026-10-02。⚠️ **2026-10-03 PIT-106 已删除自动全量校验**（用户实测"全测一遍无法忍受"）：`Probe` 只做秒级快检，全量校验仅 `--verify`/`verify` 显式触发。

### PIT-102

- PIT-102 **降级（block 失败→目录写）默认关闭：实测会系统性篡改元数据，产出"能开机但语义不对"的系统**（2026-10-02 实验证据 + 用户裁定"做不到就直接报错"，`0.6.11`）：用**复刻客户布局**（GPT：MSR + **1GB ESP** + MSR + 目标分区 1200MB@偏移 1056MiB + 数据盘）在 QEMU 直启救援层，对含隐藏+系统/只读/ADS/显式 ACL/长名/junction 的测试镜像做三组对照：
  · **block（产线）全保真**：创建时间=备份时刻、Hidden/System/ReadOnly 保留、ADS `myzone` 在、ACL 显式 Administrators+SYSTEM、短名 `LONGFI~1.DAT`、junction tag `0xa0000003`；
  · **dir(ntfs3) 与 dir(FUSE) 全部丢失/篡改**：创建时间=还原时刻、Hidden/System/ReadOnly 丢失、ADS 丢失、**ACL 变 `Everyone 完全控制`**、8.3 短名丢失、junction 变 symlink（ntfs3）/普通文件（FUSE）。
  · 同时证明**"非标布局"不是 block 失败的原因**：同一布局 block 全绿（mkntfs + hidden sectors + `apply rc=0 fallback=0` + ESP 子镜像恢复 + RESTORE DONE）。
  **策略（最终，2026-10-02 调研后定稿）**：**彻底不提供降级方案** —— `block`（wimlib NTFS 卷模式经 libntfs-3g 直写）是唯一对用户开放的还原通道。同行方案对照：wimlib 官方 = `mkntfs` + `wimapply <wim> <idx> /dev/sdX`（NTFS 卷模式；目录模式明确写着"只在元数据不重要时用"）；Clonezilla/Rescuezilla = partclone/ntfsclone **块级**克隆；Windows 侧工具（DISM/WIMGAPI/傲梅/易数）= Win32 或自家驱动 —— **没有任何一家用"挂载目录逐文件写"还原 Windows 系统**（ntfs-3g 本身没问题，是"挂载目录写"这个通道没有 Windows 元数据概念）。故 `block` 失败即 **fail-closed**：报 rc/错误码 + `say_dmesg` + 完整日志落盘，提示改用 PE 就地还原；GUI/CLI **不提供**「允许降级」选项（0.6.12 曾短暂加入、0.6.13 移除）。`zjapply=dir|dirfuse`（内核 cmdline）仅保留为**实验室对照钩子**（复现 PIT-102 对比实验用，用户流程永远不可能走到）。另：`get_task/get_log` 取值统一去掉尾部 `\r`（防 CRLF 契约静默失配）。✅ 2026-10-02（`0.6.13`）

### PIT-103

- PIT-103 **版本混淆：客户"换了新版"实际仍在跑旧救援层（日志又被 PIT-099 截断）→ 救援层加"构建版本戳"**（2026-10-03，`0.6.14`）：2026-10-03 用户日志再次停在 `found image`，据此判定为**旧版**（0.6.8 修复 PIT-099 后日志不会截断）。典型原因：只换 exe 不换 `bootfiles/`（救援 initramfs 来自 exe 目录树）、或仍点旧文件夹/旧快捷方式。以往日志头只有内核版本，**无法区分 Windows 侧与救援层各自的版本**。**修复**：`build-debian-rescue.py` 构建时把 `src/common/version.h` 的 `SYSRECOVER_VERSION` 写进 initramfs（`/zjrescue-version`），`bootfiles/alpine/init` 启动即 `say "rescue build <ver>"` —— 以后任何救援日志一眼可判救援层版本；配合 `SysRecover.exe version`（GUI 标题栏同名）即可确认整包一致。✅ 2026-10-03

### PIT-104

- PIT-104 **运行目录搬迁：日志统一到「程序目录\logs」（用户 2026-10-03 规格，`0.6.15`）**：此前程序在光盘/写保护介质上时 **Windows 侧日志完全写不出**（logger "写失败静默忽略"），救援侧才回退 `<数据盘>\ZJRESTORE`，日志散落两三处、支持成本高。新规则（用户拍板）：
  · 正常（固定盘 / PE 的 X:）→ 原地运行；日志/契约/救援回写全部在 `<程序目录>\logs\`（旧回退仅在"程序在还原目标盘"时保留）；
  · **光盘 / U盘**（可写也弹）→ GUI 启动即询问（默认「复制并运行」、可「留在原处」）；**CLI 一律自动搬、不询问**；
  · 目的地 = **装了 Windows 的分区之外的第一顺序可写固定分区**（系统在 C: 选 D:、在 D: 选 C:；PE 下用 `\Windows\System32\winload.exe` 探测、同样避开离线 Windows 分区，防止搬进将来会被格式化的还原目标）；只有系统分区时退回它，GUI 文案注明"还原系统盘时程序与日志会被覆盖"；
  · 整包复制（跳过 `logs\`，目的地日志只增不减；覆盖前归零文件属性，PIT-067/068 教训）；复制完由原进程启动新副本（GUI 不等待、单实例互斥由副本接手；CLI 等待并透传退出码）；"盘符错乱找不到日志"不成立：**救援按分区扫描找 `software_dir`、不依赖盘符**（PIT-035/099）。
  · 实现：`src/common/relocate.{h,cpp}`（目的地选择为纯逻辑 `SelectRelocateDrive`，单测 `relocate_drive_choice`）；GUI 入口 `main_win.cpp`（复用 `CConfirmDlg::Ask2`，owner=nullptr，启动早期、单实例检查之后）、CLI 入口 `main.cpp`（`InitI18n` 之后、命令分发之前）。✅ 2026-10-03（`make check` 32 用例/214 断言、i18n 360 keys、check-widths OK；**待真实光盘/U盘实测**）

### PIT-105

- PIT-105 **软件装在系统盘时日志会随系统盘一起被格式化 → 日志根改到数据盘**（2026-10-03 用户规格，`0.6.16`）：PIT-104 把日志统一到程序目录后暴露一个洞——程序装在 `C:\Program Files` 之类时，Windows 侧日志（`SysRecover-*.log`/`history.jsonl`/BCD 备份/`crash\` dump）全在 C:，而**还原/重装系统盘会格式化 C:** → 日志随程序一起消失（救援日志本来就会落 `D:\ZJRESTORE`，Windows 侧不会）。**修复**：新增 `LogBaseDir()`（`src/common/relocate.{h,cpp}`）——软件在**系统盘**上 → `<数据盘>\ZJRESTORE`（优先**不含 Windows** 的盘：PE 下避开离线 C:；优先 D:，其次第一块非系统固定盘；都不可写才退回程序目录）；其他位置 → 程序目录。GUI/CLI 的 `LogInit`/`ProgressInit`/`InstallCrashHandler`/`AppendHistory`/`diag --zip`/`history` 全部改走它，与救援层 `software_dir` 的既有回退（PIT-059）落在**同一个文件夹**；`diag` 新增 `log_dir=` 行。✅ 2026-10-03（`make check` 32 用例/214 断言；实测：程序放 C: 跑 CLI → 日志确实落 `D:\ZJRESTORE\logs`、C: 不产生 logs；**待"软件装系统盘 + 还原系统盘"实测**）

### PIT-106

- PIT-106 **自动"全量校验"太慢 → 只保留秒级快检；全量校验仅显式 `--verify`/`verify` 触发**（2026-10-03 用户实测反馈，`0.6.17`）：PIT-101 把 `wimlib_verify_wim`（**全文件扫描**）并进了 `WimEngine::Probe`，于是**每次还原暂存前**都要先把整个镜像扫一遍（20GB+ 要十几分钟），备份收尾默认也全扫。用户实测 0.6.13 后明确："正式操作之前的测试镜像时间太长……全部测试一遍无法忍受，删除这种长时间测试"。**修复**：`Probe` 回到**秒级快检**（普通打开 + `write_in_progress` + `image_count`）——PIT-057 的"半截镜像"防护保留；备份收尾默认同样只快检（阶段标记改 `probe`）；**全量校验只在用户显式要求时跑**：CLI `backup --verify` 与 `verify` 命令（`WimEngine::Verify`，带 `VERIFY_STREAMS` 进度）。**取舍**：数据级损坏但未置"写入未完成"标记的镜像不再被自动拦住（概率低；有疑虑时显式 `verify` 一次）。还原暂存前的检查从此是毫秒级，不再出现"点开始后干等十几分钟"。✅ 2026-10-03（`make check` 32 用例/214 断言；**待用户实测确认提速**）

### PIT-107

- PIT-107 **自诊断/痕迹收集全自动：启动即写 diag.txt/list.txt 到 logs、自动收集部署痕迹到 logs\collected（用户排错只管发 logs 文件夹）**（2026-10-03 用户规格，`0.6.18`）：此前让用户敲 `diag --zip`、`list`，还得满盘找我们留在盘根/`ZJRESTORE\` 的文件（grldr/menu.lst/_zjresy 日志/bootfix…），繁琐且容易漏。**修复**：新增 `src/app/selfdiag.{h,cpp}` ——
  · `WriteDiagFiles(logsDir)`：GUI/CLI 启动即把 `diag.txt`（固件/Secure Boot/启动项/工具路径/`log_dir=`/SMART 原始值）与 `list.txt`（磁盘/分区整表）写进 logs；`diag`/`list` 命令保留（同源实现，开发用）。
  · `CollectDeployArtifacts(dst, drive)`：启动扫所有固定盘、暂存完成后立即收目标盘——盘根 `grldr/grldr.mbr/menu.lst/restore-task.*/_zjresy*.log/zjrestore-boot.log` + `<盘>\ZJRESTORE\{bootfix,scripts,logs}` 子树（单文件 >32MB 只进清单）+ `ZJRESTORE-listing.txt` 整树清单 → `logs\collected\<盘>\`；覆盖时归零属性；**跳过等于日志根的那个 ZJRESTORE**（软件在系统盘时 = `<数据盘>\ZJRESTORE`，否则会把 logs 自己吞进去）。
  · 踩坑：`logs\collected` 顶层必须先 `CreateDirectory`，否则 `CreateDirectoryW(...\collected\D)` 因父目录不存在而失败 → 收集**静默全废**（实测抓出）。
  · 效果：用户排错只需发 **logs 文件夹**（+ 失败屏摄 / explorer 转储截图），`diag --zip` 不再需要用户敲。✅ 2026-10-03（`make check` 32 用例/214 断言；实测：盘根放 `zjrestore-boot.log`/`_zjresy*.log`/`ZJRESTORE\logs` → 启动自动收进 `logs\collected\D\` + 清单生成 + 日志行 `deploy artifacts collected: 4 file(s)`）

### PIT-108

- PIT-108 **客户磁盘结构复刻 + 结构自检 PASS：排除"分区结构"是幽灵卡死的原因**（2026-10-03 排查用；**无产品代码改动**）：客户机（Win10 19044.1319）Linux 块模式还原后 explorer 首启动卡死（"重启 explorer 就好、开机复发"），怀疑点之一是它非常规的 GPT 布局。做法与结论：
  · 由 `list.txt` + 契约反推**精确到扇区**的布局：Netac 256G = ESP 1GiB **@LBA40** + MSR 16MiB + C: **@LBA2129960**(1090539520B)/497988199 扇区（C: 尾+1 正好 = 备份 GPT 起点 → 整盘 500118192 扇区）；数据盘 HKVSN 2T = D300G/E500G/F550G/G≈557.7G。
  · 新增工具：`tools/vmtest/mk-customer-layout.py`（diskpart 建动态 VHD → `\\.\PhysicalDriveN` 直写手搓 GPT + mtools 造 ESP 镜像；只写非零块，VHD 不膨胀）、`tools/vmtest/prep-cust-disks.ps1`（C:/数据分区快格 NTFS；drill 盘再放 `_zjresy` 契约到 C: 根）、`tools/vmtest/run-cust-drill.ps1`（OVMF → ESP `startup.nsh` → 救援内核 → 全链断言）。
  · **结果 6/6 PASS**：`found log (dev=/dev/sda3)` → `target from log location: /dev/sda3` → `mkntfs /dev/sda3 (start_lba=2129960)` → `hidden sectors = 2129960 (OK)` → `mode=block apply rc=0` → `esp restored (rc=0)` → `RESTORE DONE`（TCG 下 26s）→ **"结构导致还原异常"排除**（含 ESP@LBA40、C: 非 1MiB 对齐、C: 顶到备份 GPT 这些非常规点）。
  · 产物（供 VM 复现第三方场景）：`tools/vmtest/base/custdisk0.vmdk`（系统盘：ESP 空 FAT32 + C: 空 NTFS，结构同客户）+ `custdata.vmdk`（D/E/F/G 四个空 NTFS）+ 对应 `.vhd`；drill 盘 = `custdrill.vhd`。
  · 备注：同一 drill 有一次卡在内核 1.8s（宿主对刚重写过的 VHD 的瞬时 I/O 抢占），重跑即通过——与产品无关。

### PIT-109

- PIT-109 **「日志」按钮 + `support` 命令：一键支持包（各盘日志 + explorer 转储 + 事件日志 + 安全模式引导）**（2026-10-03 用户规格，`0.6.19`）：排错时要发的东西太散，把"让用户做的事"全部自动化：
  · **GUI**：还原模式第三步新增「日志」按钮（静默模式与清除引导之间；静默模式左移到 x360 腾位），悬停提示"搜集还原日志与诊断信息并打包到桌面（含 explorer 转储、事件日志）"。点击后 worker 线程收集，完成后弹结果框：左「完成」（顺手用资源管理器选中刚导出的包）/ 右「重启进安全模式再测」（当前已在安全模式时右按钮变「退出安全模式并重启」）。⚠️ 结果框文案必须压成**3 行短句**（`CConfirmDlg` 只显示前 3 行，PIT-093；曾把安全模式问题拼在第 6 行导致整段没显示——`0.6.20` 修复）；按钮文字宽度上限：左 126px / 右 156px（GDI 实测「重启进安全模式再测」=126px 放得下，12 字版 168px 会裁）。
  · **支持包内容**（`src/app/selfdiag.cpp::BuildSupportBundle`）：①当前日志目录递归（含 `collected`）②**所有固定盘** `<X>:\ZJRESTORE\logs`（覆盖"单文件/temp 运行、日志落数据盘"）③**explorer 进程转储**（`common/crash.cpp` 新公开 `WriteProcessMiniDump`，MiniDumpNormal|WithThreadInfo = 含全部线程栈、体积小）④**事件日志**文本（wevtutil：Application Hang/Error、System 存储错误、System 错误级；PE 无 wevtutil 自动跳过）⑤diag.txt/list.txt/version.json/bundle-info.txt → `SysRecover-logs-<时间>.zip`。默认输出到**桌面**（`SHGetFolderPathW` 自动尊重重定向的桌面路径，实测本机落到 `E:\我的下载\DESKTOP.INI\桌面\`）；`--out` 可指定。
  · **安全模式**（`boot/bcd.cpp::BcdSetSafeBoot`）：`bcdedit /set {current} safeboot minimal` ⇄ `/deletevalue ...`；进入/退出两个方向都在，提示里带 BitLocker 警告（可能要恢复密钥）。注意 safeboot **持续有效**，必须靠按钮的第二方向退出（或用户 msconfig 取消）。
  · **CLI**：`support [--out <zip>]`（PE/批处理/开发用）与 `help support`。
  · ⚠️ **i18n 工具坑（本批实测）**：`src/app/selfdiag.cpp` 此前**不在** `tools/i18n-wrap.py` 的 `WRAP_FILES` 里 → `--skeleton` 扫不到它的词条，**还会把它们当死键从译文表删掉**（实测丢了 `ON (需签名引导)`、CA2023 警告两条）。已加入白名单；以后新增含 Tr 的源文件**必须同时加白名单**。
  · 实测：CLI `support` 产出 1.31MB 包（logs=23 / dumps=2 / events=4）；GUI 点击冒烟全链通过（日志有 `support bundle exported`）。✅ 2026-10-03（`make check` 32 用例/214 断言、i18n 379 keys、check-widths OK；`make package` **0.6.19**）

### PIT-110

- PIT-110 **"暂存+重启"还原从不自动重启、还显示"还原完成"（元凶：`needReboot` 被无条件清零；自 ≤0.6.4 起就存在）**（2026-10-04 客户支持包日志定位，`0.6.21`）：客户 0.6.20 日志显示 `restore mode: staged reboot` + `restore staged, reboot to execute`，但 GUI 状态栏却是 **"还原完成（用时 00:01）"**、且**没有自动重启**（用户描述"点了开始后中途停止了"）。根因：`ops.cpp::StageRestoreImpl` 返回前一行 `if (needReboot) *needReboot = false;`（注释写"菜单项模式不重启"，但没有 `menuEntry` 条件）→ 暂存路径也被清成 false → GUI/CLI 走"就地完成"分支（弹"系统还原已完成…重启后即可进入恢复的系统"、**不调 `RebootNow()`**）。**0.6.4（已提交版）里就是同一行**，即所有"暂存+重启"还原实际都要用户**手动重启**；10-03 客户截图"还原完成（用时 01:04）"当时被误判成"就地还原"，实为暂存路径。**修复**：`if (menuEntry && needReboot) *needReboot = false;`（暂存保留 true ⇒ 提示"暂存完成（用时…），正在重启..." + `RebootNow()`，与 PIT-072 规格一致）。⚠️ 排查启示：`bool` 出参"默认由调用方初始化"时，被调方**无条件清零**等于废掉调用方默认值——此类语义建议显式命名（如 `outNeedReboot`）或改为枚举。✅ 2026-10-04（`make check` 32 用例/214 断言；`make package` **0.6.21**；**待真机复测自动重启**）

### PIT-111

- PIT-111 **救援层「黑匣子」：任何失败都必须留下文件 + 开机即报 + 收集后清理 + 日志总量上限**（2026-10-04 客户三盘机"脚本快速 rc=1、屏幕外零证据"的教训；用户 5 条规格，`0.6.22`→`0.6.23`）：0.6.20 在那台机上救援脚本 rc=1 退出，console 之外**没有任何文件证据**（软件目录找不到就全丢），只能靠录像逐帧分析。改造（按用户规格 1–5）：
  · **探测表**（规格 1/2）：Windows 启动把盘/分区表写进主日志（`LogFileInfo("inventory: …")` —— 新增 `LogFileInfo` **只进文件不回显控制台**，避免污染 CLI stdout 与脚本解析）+ `collected\drive-map.txt`（盘符↔diskXpY/偏移/大小/serial 对照表，解决 PE 与正常系统盘符互相打架）；Linux 侧每个分区的 mount/open 结果进 `/tmp/zj-probe.txt` **且镜像进主日志**（`PROBE …` 行，init 与脚本共用）。
  · **多面落盘**（规格 3）：`zjrestore-lite.sh` 黑匣子把 `ZJRESTORE-last.log`（完整日志）+ `ZJRESTORE-probe.txt` + `ZJRESTORE-status.txt`（回执：result/step/target/image/build）+ mkntfs/apply/esp 输出写到**每个可挂载分区根**（失败全量扫；成功只写软件目录 + ESP，**不碰还原后的目标盘**）；ESP 有 `\EFI\ZJRESTORE` 时写进其 `logs\`。软件目录找不到时**尽早认领兜底日志窝** `ZJRESTORE-logs\`（非目标分区、保持挂载、持续镜像 2s，防中途断电无日志）；`/tmp/zj-bb.done` 记录已写设备，init 在"脚本没来得及写盘"时调 `zjrestore-lite.sh zz-blackbox <reason>` 兜底（逻辑单源）。**修掉误导消息**：原来无条件打印 `no restore task -> shell`；现在按真实结果打印 `RESULT: restore FAILED (rc=…)` + 日志位置（脚本 rc≠0 = 任务找到了但脚本失败）。
  · **目标校验防劫持**：采信"日志所在分区 = $1"前用契约 `target_part_offset` 校验设备实际偏移；不符（参照机曾出现 `target from log location: /dev/sda1`，被别处旧 `_zjresy` 劫持）→ 拒绝并回退 offset 精确匹配。镜像扫描加第二轮（3s 后重试，防枚举晚/瞬时挂载失败）。
  · **失败回执 + 开机即报**（规格 2）：`selfdiag::CheckLastRescueFailure()` 扫 `logs\collected` 里 `result=FAILED` 的回执 → GUI 启动弹 3 行提示（失败步骤中文化/时间/版本/日志位置），左「知道了」右「打开日志文件夹」（`WM_APP+15`；GUI/CLI 共用去重标记 `logs\.last-rescue-status`，同一回执只报一次）；CLI 启动在 stderr 打同样提示。
  · **收集后清理**（规格 4）：`support`/「日志」出包成功后 `CleanupStrayLogs()` 删各盘根黑匣子三件套 + `zjrestore-boot.log` + `ZJRESTORE-logs\` + 非活动 `<盘>\ZJRESTORE\logs\` + ESP `\EFI\ZJRESTORE\logs\*`（契约 `restore-task.*`/`_zjresy*` 与引导文件**不动**）。
  · **总量上限**（规格 5，二次修订）：`logger.cpp::PruneBySize()` —— logs 目录（含 crash/collected，深度≤3）超 **32MB** 触发：**先删一周以上的旧日志**；仍超 32MB 则最旧优先删到 **8MB 以内**；当前活动日志永不删（文本日志一次运行仅数百 KB，8MB 足够多次运行）。
  · **回归**：QEMU drill 成功路径（黑匣子进 ESP + 软件目录、`RESTORE DONE`、盘上 `ZJRESTORE-status.txt result=OK`）+ **造坏演练**（conf 改指向不存在的镜像）：`PROBE` 全表、`target … (offset verified)`、两轮扫镜像、`blackbox: log -> /dev/sda1|sda2|sda3`、init 打印 `RESULT: restore FAILED (rc=1)`，三个分区上均有 `ZJRESTORE-status.txt`（`result=FAILED step=image-not-found`）；Windows 侧合成回执冒烟（第一次提示/第二次静默/标记正确）。`make check` 32 用例/214 断言 + i18n **393** keys + check-docs PASS。✅ 2026-10-04（`0.6.22`–`0.6.24`；**待真机失败场景复测**）

### PIT-112

- PIT-112 **三盘客户机全真复刻（MA 0902 2T MBR + ST1000 1T MBR(扩展分区) + KIOXIA 250G GPT）：端到端还原 6/6 PASS；复刻过程抓出并修掉两个真问题**（2026-10-04，`0.6.25`）：
  · **工具**：`tools/vmtest/mk-3disk-layout.py`（diskpart 动态 VHD + 手写 MBR/EBR/GPT + mtools 造 ESP；参数全取自支持包 list.txt/契约，精确到 LBA：目标 @2129960=1090539520B）+ `prep-3disk.ps1`（格式化 + 投放客户**原文契约**与 F: 的 `九转还原/` 软件目录+镜像）+ `run-3disk.ps1`（QEMU q35：ST1000=sda、MA=sdb、KIOXIA=nvme0n1；`ide-hd` 带 model/serial 复刻品牌；6 项断言；检测到"救援 shell"提前收工）。产物 `base/3disk/*.vhd`（动态 VHD，各 ~250MB）。
  · **验证**：0.6.25 全链 PASS（`found log nvme0n1p3` → `offset verified` → 镜像在 F: 找到 → `apply rc=0` → `esp restored` → `RESTORE DONE`）；0.6.20 的原失败**不可复现**（与"旧版/环境瞬态"一致，但下次任何复发都有黑匣子证据）；扩展分区逻辑盘在 Linux 下是 `sda5/sda6`、扩展容器有 `sda2` 节点——正好解释客户 list.txt 的绿色幽灵 `Part 0` 与屏幕上的 `sda2` 扫描行。
  · **修复 1（开发期回归，自捕）**：新加的"目标 offset 校验"最初只用 `blkid -s PART_ENTRY_OFFSET` + MBR 兜底 → **GPT 盘返回 0**（blkid 空、保护性 MBR 读出来是 1/0）→ 会把**正确目标**当 stale log 拒掉。改为**首选 sysfs `/sys/class/block/<p>/start`**（GPT/MBR/EBR 逻辑盘全准），blkid/MBR 仅兜底。教训：GPT 上任何"分区偏移"都不能依赖 MBR 结构。
  · **修复 2（静默卡死）**：`bootfiles/alpine/init` 的 `mnt_dev` 没有 timeout（`zjrestore-lite.sh` 版有）→ 复刻首跑在某个挂载上**静默卡死 400s**（与客户"卡住"同类症状）。已给 init 全部 mount/blkid 加 `timeout 30/60`，bootfix 扫描加逐盘日志与 `bootfix scan done` 标记——挂载挂死从此变成一条 FAIL 记录而不是死机。
  · ⚠️ 复刻机还实证："**给 Windows 手写 GPT 必须带保护性 MBR 的 `55AA` 签名**"（本脚本初版漏写 → Windows 认成 RAW；QEMU/Linux 不受影响）。
  · 回归：`make check` 32 用例/214 断言 + check-docs/i18n PASS；BIOS 演练（`mk-drill.py`+`run-drill.ps1`）在新 initramfs 下通过（`RESTORE DONE` + 黑匣子进 ESP）。✅ 2026-10-04（`0.6.25`）

### PIT-113

- PIT-113 **真实镜像压测 + 小内存边界（三盘复刻环境，`0.6.25`/`0.6.26`）**（2026-10-04，用户要求"用我的本机备份镜像多测、刻意试小内存"）：
  · **真镜像全链**：Win10 ESD（7.57GB 未压缩、无 ESP 子镜像）8G 内存**全程通过**（apply 438s）；Win11 WIM（18GB 文件 → **43.35GB 未压缩** + ESP 子镜像）8G 内存**全程通过**（apply **1486s**、`esp restored (rc=0)`、ESP 回执 `result=OK step=done`）。
  · **小内存**：**512MB 无故障**（大 WIM 正常推进——PE 做不到这点）；**192MB 会 OOM**——wimlib 在 apply **最后阶段**（文件全写完、设安全描述符时）被内核 OOM 杀（rc=137，等于整盘白写）。
  · **失败留证（实测）**：`apply FAILED rc=137` + OOM dmesg 原文 + **三块盘每个分区**的 `ZJRESTORE-status.txt`（`result=FAILED step=apply-failed`）+ F: 软件目录完整日志（含 mirror）——"小内存故障是否被记录" 的答案是**全面记录**。
  · 新增**低内存预警**（`zjrestore-lite.sh`，`0.6.26`）：`MemTotal < 400MB` 时开机即打 `WARN: low memory … may be OOM-killed; increase VM RAM`，避免"整盘写完才失败还不知道原因"。
  · 工具：`mk-3disk-layout.py` 支持 `ZJ_3DISK_IMAGE/ZJ_3DISK_IMGNAME/ZJ_3DISK_ESP_INDEX` 换真实镜像；`run-3disk.ps1 -Mem/-Smp`；新增 `check-3disk.ps1`（挂载三盘打印回执与日志尾部，验收失败路径证据）。✅ 2026-10-04

### PIT-114

- PIT-114 **契约清场 + 部署校验 + 控制器清单 + 设备排序（用户 2026-10-04 A2/A5/A6/A9/A1 五条规格，`0.6.27`）**：
  · **A2 契约清场（杜绝目标劫持）**：Windows 暂存写新契约前，`task.cpp::CleanupStrayContracts()` 清掉**所有固定盘/可移动盘根**上的 `_zjresy*.log` / `restore-task.conf|json`（只留即将写入的唯一一份；不碰 `<盘>\ZJRESTORE\` 与软件目录）；Linux `init::scan_for_log` 改为**优先采用"日志里 target_part_offset == 该分区 sysfs start"的那份**（多份时不再挑错），无匹配才回退第一份并 `WARN`。script 的 offset 校验保留为第二层。
  · **A6 旧版救援混用杜绝**：BIOS/GRUB4DOS（`grub.cpp::CopyBootFiles`）与 UEFI/ESP（`uefi.cpp::InstallUefiBootEntry`）部署后**逐文件 CRC32 校验**（源 vs 落盘，不一致即部署失败）；两处均落**明文构建戳 `<盘>\ZJRESTORE\rescue-build.txt`**（= `SYSRECOVER_VERSION`；收集器会收进 `logs\collected`，现场一眼核对盘上救援版本）。
  · **A5 控制器清单**：Linux 救援层开机打印 `storage/usb controllers (pci)`（读 `/sys/bus/pci` 的 vendor:device / class / 已绑定驱动，零新依赖）；Windows `diag` 增加 `pci-ctrl` 行 —— ⚠️ `Enum\PCI` 受 ACL 保护（**管理员也读不到**，实测），改走 `Control\Class\{SCSIAdapter,HDC,USB}` 读 DriverDesc/Service/MatchingDeviceId。以后"是不是缺/错驱动"可直接对照。
  · **A9 U 盘/光盘可感知**：`list_parts`（init 与脚本）改为 **内置盘分区 → 可移动盘分区 → 无分区的可移动整盘（光盘/未分区 U 盘）** 顺序；probe 行加 `rem=0/1`；`find_esp_dev` 跳过可移动盘（U 盘上的 ESP 类型分区不再被当成目标机 ESP）；`scan_image` 命中时记录设备号+rem。**顺带修了一个真缺口**：此前 `list_parts` 只取"有分区的设备"，`sr0`/未分区 U 盘实际扫不到（"镜像可放光盘"在代码上并不成立）；现在它们排在最后但会被扫描。
  · **A1 重试分级**：目标按 offset 匹配失败 → 3s 后重试一轮；镜像扫描第二轮 3s，**仅当出现过挂载失败**再第三轮 10s（`/tmp/zj_img.fail` 标记）——覆盖瞬时 I/O 错误恢复/设备晚到，避免空等。
  · 回归：BIOS 演练（`SR: found log ... offset match 105906176` + PCI 清单 + `rem=0`）✓；三盘客户复刻 6/6 PASS（`offset match 1090539520`、PCI 列出 `nvme`/`ahci`）✓；Windows `diag` 实测输出 `pci-ctrl`（NVMe/SATA/USB）✓；`make check` 32 用例/214 断言 + check-docs/i18n PASS。
  · **复刻验证"第2盘分区挂不上"假设（2026-10-04 追加）**：`qemu-io` 把 ST1000 的 F: 分区引导扇区清零模拟"分区挂不上" → 0.6.27 完整留证：`mount FAILED /dev/sdb5 ... NTFS signature is missing`（probe 含原文）、`status=FAILED step=image-not-found` 写满所有可挂载分区 + ESP、`fallback log home` 生效、init 打印真实结果——**同一故障在 0.6.20 只会剩看不见的屏幕行**。
  · **复刻验证 A2 契约选择（2026-10-04 追加）**：目标根同时放 `_zjresy-fake.log`（offset 不符、字母序在前）+ 真契约 → init 仍选中真契约（`offset match 1090539520`、无 fallback 警告），全链 6/6 PASS（若挑错，offset=999999999 会直接 target-not-found，故具备判别力）。
  · **R4**（`0.6.28`）：GUI `RebootNow` 的 `ExitWindowsEx` 与 `shutdown.exe` 两条路都**记返回码**（含特权启用/`GetLastError`）——"说重启却没动"从此有账可查。✅ 2026-10-04（`0.6.27`/`0.6.28`）

### PIT-115

- PIT-115 **证据覆盖审计：补上"暂存成功但救援从未执行"的静默空洞（待执行标记 + 启动比对）**（2026-10-05，`0.6.29`）：审计整个失败面后发现**唯一可操作的洞**——"引导失败/开机断电导致救援层从未跑过"时，磁盘上没有任何 `ZJRESTORE-status.txt`，启动检查自然什么都不报（用户视角："点了开始，重启后什么都没发生"）。修复：① **暂存+重启**路径写 `<日志根>\logs\pending-restore.txt`（time/target/image；**菜单安装不写**，避免常驻菜单误报）；② 启动检查改为"最新回执时间（**含 OK**）vs 标记时间"：标记更新（或压根没回执）→ 提示"上次系统还原任务未执行（可能未进入恢复环境）"+ 删除标记（只报一次）；有更新回执 → 走原有 FAILED/成功逻辑并删标记。合成用例实测：T1（标记最新 → "未执行"提示 + 标记删除）、T2（标记旧 + FAILED 新 → 正确走失败回执）。**单测（`0.6.33`）**：判定逻辑抽到 `src/common/rescue_decision.h`（纯函数 `DecideRescueReport`），`rescue_report_decision` 9 场景断言——**单测当场抓出一处误判**：只填 FAILED 时间、未填 `latestStatusTime` 时会把 FAILED 当"无回执"→ 已改为取两者最大值（对不完整输入稳健）；顺带把该头文件加进 `TEST_BIN` 依赖（此前改 `.h` 不触发重编，测试跑的是旧二进制）。`make check` **33 用例/223 断言** + i18n **396** 键全绿。✅ 2026-10-05（`0.6.33`）

### PIT-116

- PIT-116 **"屏幕也是证据面"：无文件可写 / 内核级挂死的表达 + 失败日志尾部重打 + 超时标记（用户 2026-10-05 规格，`0.6.30`/`0.6.31`）**：
  · `mnt_dev`（init + 脚本）每次尝试前打印 `mnt <dev>` —— 挂死时最后一屏就是"卡在哪个设备"（无文件可写时的唯一线索）。
  · `timeout 60` 命中（rc=124）时把 `TIMEOUT(60s:ntfs3/ntfs-3g)` 写进 `/tmp/zjmnt.err` → 进 probe 行：区分"挂死超时"与"普通失败"。
  · 失败时 init 把 `ZJRESTORE-last.log` 的**最后 20 行重打到屏幕**（先快照再打头，避免头部被 tail 自读）；**全盘不可写**时打 `===== PHOTOGRAPH THIS SCREEN =====` 横幅 + 日志尾 25 行 + 内核消息 20 行。
  · 脚本 `bb_final` 失败分支补 `say_dmesg`：内核 I/O 错误 / ata / nvme reset 进黑匣子日志（"任何失败都要能从记录中评估"）。
  · 局限（诚实记录）：真正的内核冻结/断电无法"事后打印"（只能靠最后一条 `mnt` 标记定位）；`timeout` 对 **D 状态**（不可中断 I/O）挂载杀不动——同样靠 `mnt` 标记定位，不静默。✅ 2026-10-05（`0.6.30`/`0.6.31`；QEMU 失败演练实测 `---- log tail (last 20 lines) ----` 块与 `RESULT: restore FAILED` 正常、黑匣子回执不变）

### PIT-117

- PIT-117 **支持包带屏幕截图 + diag 补整机/BIOS/环境变量 + 救援层控制台文本转储；日志上限提到 64MB/16MB（用户 2026-10-05 规格，`0.6.32`）**：
  · Windows：`support`/「日志」出包时 GDI `BitBlt` 抓桌面 → `logs\screens\screen-<时间>.bmp`（24bpp BMP，1080p 约 6MB；零依赖、PE 可用；随 logs 递归进包）；`diag` 新增 `bios`（整机/主板厂商型号、BIOS 版本/日期，读注册表）、`env`（PROCESSOR_ARCHITECTURE/SystemRoot/windir/TEMP）与 `locale` 行。
  · 救援层：开机打 `dmi:`（sys_vendor/product_name/board_name/bios 版本日期）与 `cmdline:`；失败时 `dump_console` 读 `/dev/vcs*`（VT 字符矩阵 → tr 去 NUL + fold 折行）把**屏幕文本**写进黑匣子日志（QEMU 实测抓到 52 行真实内核消息；生产 cmdline 带 `tty0` 必有内容）。
  · 日志上限（第三次修订）：`>64MB` 触发 → 先删一周前 → 仍超则删到 **16MB** 以内（加截图/更多文本后仍余量充足）。
  · ⚠️ 构建坑：CLI 链接需补 `-lgdi32`（截屏用到 GDI；GUI 早就链接了）。
  · 回归：支持包实测含 `logs/screens/screen-*.bmp`（6.0MB）+ diag 的 `bios`/`env` 行；三盘失败演练含 `console text (/dev/vcs)` 且内容非空。✅ 2026-10-05（`0.6.32`）

### PIT-118

- PIT-118 **客户 0.6.33 实测抓出：`mnt` 屏幕标记污染命令替换 → ESP 子镜像恢复/成功回执失效（`0.6.34` 修复）；同批收获大量真机验证**（2026-10-05）：
  · **根因**：PIT-116 的 `mnt <dev>` 标记用 `echo` 走 **stdout**，而 `ESP_DEV=$(find_esp_dev)` 会把函数内所有 stdout 一起捕获 → `ESP_DEV` 变成 `"ZJ:   mnt /dev/sda1\n/dev/sda1"` → 后续 `mnt_dev` 必然失败。客户机（GPT ESP）的 Alpine blkid **没返回 `PART_ENTRY_TYPE`** → `find_esp_dev` 走 **FAT 回退**（此路径会调用 `mnt_dev`）→ 触发；QEMU 演练盘 blkid 能返回 GUID → 走 GUID 路径（不调用 mnt_dev）→ 此前所有 drill 全绿也没暴露 ✗。
  · **影响**：① ESP 子镜像恢复失败（`ERROR: esp mount failed`，非致命；因 ESP 未被改动 + 暂存时 bcdboot 已刷新 C: 引导文件，系统照常启动）；② 成功路径的状态回执（原先只写 ESP）一并失败 → 下次启动会把 pending 标记**误报成"任务未执行"**。
  · **修复**：① 两个脚本的 `say()` 一律走 **stderr**（`1>&2`；屏幕/串口照旧，禁止再污染 `$(...)`）；② 成功路径把 `ZJRESTORE-status.txt` **也写进软件目录 logs**（BIOS 无 ESP 也保证有 OK 回执）；③ Windows `CheckLastRescueFailure` 增加扫描日志根直下；④ `run-drill.ps1` 新增 7 项断言（含 `esp restored` + `blackbox ESP`）——此前只打印不判定，是这次静默漏检的原因。
  · **真机实证**（客户 LENOVO 90MU000CCD，Netac 256G + HKVSN 2T GPT，**7-Zip SFX 单文件包在 `%TEMP%\7ZipSfx.001` 运行**）：VSS+ESP 备份 ✓、暂存 ✓、**自动重启（`reboot: ExitWindowsEx ok`）✓**、救援全链 `apply rc=0`（12.7GB/123s）✓、`RESTORE DONE` ✓；A2 清场（0 stray）、DMI（`dmi: LENOVO | 90MU000CCD`）/PCI 清单、pending 标记、sysfs offset 校验（该盘 blkid 偏移也为空，靠 sysfs 救回）均在真机落地。✅ 2026-10-05（`0.6.34`）

### PIT-119

- PIT-119 **客户 0.6.33 复测反馈两件事：①还原后 explorer/右键卡死（新备份里原样复现）②PE 还原把易数启动项干掉了 —— 修 bcdboot 重建 BCD + 新增壳扩展审计（`0.6.35`）**（2026-10-05）：
  · **背景**：客户重新备份（0.6.33）→ 还原 → 症状不变（登录后 explorer 卡、任务管理器重启 explorer 才能用；**桌面右键必转圈**、左键正常）。"重新备份再还原仍复现" → **故障在源系统里**（我们的还原是如实的）；右键菜单卡死是**第三方 shell 扩展死锁**的教科书症状；救援日志可见 C: 根有 `Coodesker`（酷呆桌面）——头号嫌疑（易数/火绒外壳项次之）。
  · **修复 1（BCD 第三方条目保护）**：`bcdboot` 会**重建 BCD**（只剩 Windows 条目），易数等第三方启动项被抹掉。三条路径（PE 就地 UEFI/BIOS、暂存 UEFI）全部改为：**先判定 BCD 已有效就跳过 bcdboot**（`VerifyEspBcd` / 新增 `BiosBcdLooksValid`；同机还原原 BCD 本就指向同一 C:），只有缺失/损坏才重建，且重建前 `BcdExport` 备份为 `bcd-backup-before-bcdboot`。
  · **修复 2（壳扩展审计）**：`diag` 新增 `shellext` 行 —— 枚举右键处理器（`*`/`AllFilesystemObjects`/`Directory`/`Directory\Background`/`Drive`/`Folder`/`ThisPC`）/ 图标叠加 / `ShellExecuteHooks` / 被屏蔽项，CLSID→`InprocServer32` DLL 并查文件存在。**只有"解析到真实路径但文件没了"才标 `*** MISSING ***`**（Windows 内建无 InprocServer32 的形态不当故障）；两种注册形态（子键名=CLSID 或 默认值=CLSID）都识别——本机实测出 7-Zip/火绒外壳项与内建项的正确 DLL。
  · 待复测：0.6.35 客户机暂存日志应出现 `skip bcdboot (preserve third-party...)`、易数条目保留；壳扩展排查需客户在卡死系统上跑 `support` 发回（先停用 **Coodesker** 试验，安全模式对照）。✅ 2026-10-05（`0.6.35`；bcdboot 跳过逻辑待真机复测）

### PIT-120

- PIT-120 **客户"别的软件还原没事、我们还原后右键卡死"的实锤差异：Linux 侧 wimlib 丢 NTFS 扩展属性（EA），Windows 侧不丢（`0.6.36`）**（2026-10-05）：
  · **证据 1（客户）**：0.6.33 的 `zjrestore-apply.out` 里唯一一条警告：`[WARNING] Ignoring extended attributes of 804 files`。
  · **证据 2（二进制）**：`Ignoring extended attributes` 字符串只存在于 **Linux 侧 wimlib**（initramfs 的 wimlib-imagex），**Windows 的 libwim-15.dll 里没有**（Windows 侧支持写 EA）。
  · **证据 3（实验室，可复现）**：新增 `tools/ea-scan.cpp`（`--set` 用 `NtSetEaFile` 造 EA；无参扫描列出带 EA 文件）。造 `ea1.bin`（EA `ZJTEST=1`）→ 我们工具 `backup`（Windows）→ `extract`（Windows wimlib）→ `fsutil file queryea` **EA 仍在** ✅。结论：**PE 就地还原（Windows 侧 wimlib）保 EA；暂存重启（Linux 侧 wimlib）丢 EA** —— "别人没事、我们有事"的保真度差异坐实。
  · **取证命令**：新增 `SysRecover.exe scan-ea [--root <目录>] [--out <文件>]`（`selfdiag.cpp::ScanEaFiles`，`NtQueryEaFile` 列出路径 + EA 名/值前 32 字节）→ 客户在 PE 还原后的系统上跑一次，即可拿到那 800 个文件的名单。
  · **BCD 保护（同批）**：`bcdboot` 重建 BCD 会删第三方条目（客户 BCD 备份实证：`Boot Windows created by 石头`、`Windows_PE`、UEFI 设备条目等）。三条路径改为 **BCD 有效即跳过 bcdboot**（`VerifyEspBcd`/`BiosBcdLooksValid`）；重建前 `BcdExport` 备份。客户手上 `logs\bcd-backup` 可用 `bcdedit /import` 找回条目。
  · 待定：EA 丢失与"右键卡死"的**因果**需客户交叉验证（0.6.36 的 PE 就地还原 → 右键应恢复；或 Dism++ 应用我们的镜像对照）。若坐实，Linux 侧 EA 恢复方案（捕获 sidecar + 首启补写）另行设计。✅ 2026-10-05（`0.6.36`；实验室差异已复现）

### PIT-121

- PIT-121 **三项小修：①「删除菜单」后按钮不回「安装菜单」②备份完成自动做 EA 审计（清单落日志）③多款还原软件对照信息（`0.6.37`）**（2026-10-05）：
  · **① 根因**：`RefreshBootMenuBtn` 的"已安装" = `BootMenuInstalled()`（引导条目/文件）**或** `ReadMenuBinding()`（读 `<exeDir>\restore-task.conf`）；「删除菜单」只删了引导条目/文件，**没删副契约** → 永远判"已安装"，按钮停在「删除菜单」。修复：新增 `ops::DeleteMenuBinding()`（删 conf/json + 清 pending 标记），「删除菜单」与「清除引导项」都调用并刷新按钮。
  · **② 自动取证**：`RunBackup` 收尾自动调用 `ScanEaFiles(源)`（阶段名 `ea-scan`，状态栏中文"文件属性检查"），清单写 `<logs>\ea-scan-<时间>.txt`；发现 EA 时 `WARN`（附"Linux 救援还原会丢 / PE 不丢"）。实测全盘 C: **30.2 万文件仅 15 秒**，用户零操作；开发机 0 个 EA（客户机的 804 个来自其特定软件，名单下次备份自动拿到）。
  · **③ 客户对照**：**多款还原软件（易数等）均正常，只有我们的（Linux 救援路径）有问题** → 与 PIT-120 的 EA 差异互相印证；下一步让客户用 **0.6.37 在 PE 就地还原 `20261005Win10.19044备份.wim`**（Windows 侧 apply 保 EA + 不再重建 BCD），右键应恢复，随后 `scan-ea` 出名单即定案。✅ 2026-10-05（`0.6.37`）

### PIT-122

- PIT-122 **EA（NTFS 扩展属性）全自动修复链路落地：备份采集 → ZJEA 子镜像 → 救援投放 → 首启补写器（`0.6.38`，2026-10-06）**：Linux 侧 wimlib apply 丢 Windows EA 是 PIT-120 实锤的客户"还原后桌面/右键卡死"根因差异；"Linux 侧直接写回 EA"经两组 QEMU 实验**否决**（`ntfs-3g user_xattr` 把 `user.*` 写成 **ADS 流**而非 EA；对真 EA 读不到、`system.ntfs_ea` 写被拒）→ 改为**首启补写**（Windows API 写 EA 是唯一可行通道，PIT-123 提供钩子）。链路与落点：
  · **备份端**（`src/common/ea.{h,cpp}` + `ops.cpp::RunBackup`）：`CaptureVolume(源)` 一次遍历产出审计清单（`logs\ea-scan-<时间>.txt`，沿用 PIT-121）**+ eapack.dat**（二进制格式 `ZJEA1`：每文件 u32 路径字节数+UTF-16LE 相对路径+EA 列表[名/值，值二进制安全]，值上限 65535、包上限 128MB 超限只审计）；发现 EA → 把 `eapack.dat + zj-ea-apply.exe（自 bootfiles/ 拷入）+ zj-regpol.bin（策略片段）` 打成**子镜像 `ZJEA`**（复用 ESP 的 Append 机制；`RunBackup` 的 `stage` 改为**非 append 一律暂存**，任何子镜像并入失败不损主镜像）。**非致命**：采集/并入失败只 WARN，主镜像照常交付。
  · **还原端**：`StageRestoreImpl` 从镜像内容发现名含 `ZJEA` 的子镜像 → 契约 **`ea_index`**（增量键，不升 contract_version）；名含 ZJEA 的子镜像**拒绝单独还原**（同 ESP 防呆，rc=4）。**就地还原（PE/非系统盘）无需此链**——Windows 侧 apply 原生保 EA。
  · **救援层**（`zjrestore-lite.sh`）：apply 完主系统后按 `ea_index` 解出子镜像 → 目标盘投放 `\ZJRESTORE\ea\{eapack.dat, zj-ea-apply.exe}` + 写 GPO 钩子（cmd + scripts.ini 合并 + gpt.ini 合并 + Registry.pol 追加，见 PIT-123）→ **落盘复核**（6 个文件逐个查，缺一即 WARN "deploy INCOMPLETE"，不静默）。
  · **首启补写器**（`src/tools/ea_apply_main.cpp` → `bootfiles/zj-ea-apply.exe`，**x86 构建**（x86/x64 目标都要能跑），无 wimlib 依赖，~900KB）：登录前 SYSTEM 执行 → `NtSetEaFile` 逐文件写回（`FILE_FULL_EA_INFORMATION` **链式**、逐条 4 字节对齐、末条 NextOffset=0、**无外层头** —— 对齐 go-winio/restic 实现，实验室 `--set` 单条目版已 fsutil 复核）→ 写 `ea-apply.log`（含 `shell_started=` 预登录判据）+ `ea-result.txt`（一行计数）→ **自清理**：scripts.ini 删条目、gpt.ini 版本 +1、Registry.pol 剥记录（解析失败原样不动）、删 pack、cmd/自身 `MOVEFILE_DELAY_UNTIL_REBOOT` 延迟删除（下轮启动生效；文件被占用时删不掉是 cmd 常态）→ 失败保留重试（attempts 计数，3 次后改名 `.failed` 停重试）。
  · **测试全绿**（全部由 AI 独自完成，无用户参与）：单测 6 个新用例（包往返/拒绝截断/pol 剥记录/EA 缓冲布局/ini 清理/gpt 递增）→ `make check` **39 用例/259 断言**；本机 e2e（造 EA → backup → extract → ea-apply → `fsutil queryea` 确认写回）；**QEMU 演练 9/9 PASS**（`ea subimage: index=3` → `ea: deployed (payload + GPO hook` → 盘上字节级复核：ini CRLF/双 CSE/版本/pol 382B 头全对）；**VM 首启 e2e（真实 Win10，round6~9）**：预登录执行（`shell_started=no`）、**含 `System32\` 路径**的 EA 写回成功、清理全项完成、延迟删除生效、**次轮不重跑**。✅ 2026-10-06（`0.6.38`）
  · **全链 e2e（0.6.39，2026-10-06）**：对**真实 Win10 系统镜像**（2.22GB WIM、**1677 个 EA 文件**）走完整链一次贯通：冷备采集（EA 包 563KB）→ QEMU 救援层真实还原（5.5GB apply + EA 投放 + 对**已有**钩子文件做合并：scripts.ini idx=0 / gpt.ini 双 CSE 已存在仅升版本 / pol 追加）→ 首启（预登录 `shell_started=no`）**ok=1676 / missing=1（被备份排除的文件，正常）/ failed=0** + 清理全项 + 日志可读。**唯一未在运行时跑过的**：Windows 暂存端从镜像发现 `ea_index`（与 ESP 共用同一 ListImages 循环，drill 用手写契约覆盖了后半链）。
  · ⚠️ **遗留**：客户**已有旧镜像**（EA 完好但无 ZJEA 子镜像）→ 需 Windows 侧还原（PE 就地）或后续 `ea-repack`（从旧镜像在 Windows 上物化 EA 再打包，**尚未实现**）；客户真机确认因果（首启后右键/桌面是否恢复）。

### PIT-123

- PIT-123 **本地组策略「启动脚本」当首启钩子的机理定稿 + 三个实测坑（WOW64 重定向 / gpt.ini 版本撞车 / ccs 日志编码）（2026-10-06，`0.6.38`）**：EA 补写器必须在**登录前**（explorer 加载前）以 SYSTEM 跑完，否则卡死的 explorer 已经挂了。方案 = **纯文件**写本地 GPO（免注册表编辑），QEMU 对真实 Win10 做了 9 轮实验定稿：
  · **布局**：`Windows\System32\GroupPolicy\Machine\Scripts\Startup\<cmd>` + `Machine\Scripts\scripts.ini`（`[Startup]` 段 `NcmdLine=<cmd>` + `NParameters=`，索引=全文件数字键 max+1，插入点在 Startup 段内/新段）；`gpt.ini` 的 `gPCMachineExtensionNames` **必须声明两个 CSE 对**：Scripts `[{42B5FAAE-…}{40B6664F-…}]` + Registry `[{35378EAC-…}{D02B1F72-…}]`（缺 Scripts 对 → **脚本 CSE 根本不被调用**，策略处理了也不跑脚本，Extension-List 里没有它）；`Machine\Registry.pol`（记录格式 = `[key;\0 name;\0 type(4LE);\0 size(2LE)+00 00;\0 data]\0`，头 `PReg`+u32 1）追加两条 **REG_DWORD=1**：`Software\Microsoft\Windows\CurrentVersion\Policies\System!RunStartupScriptSync` + `Software\Policies\Microsoft\Windows NT\CurrentVersion\Winlogon!SyncForegroundPolicy`（ADMX 权威值；没有它们 Win10 默认**异步**执行启动脚本 → 实测登录后 ~1 分钟才跑，赶不上）。追加 pol 片段要**剥掉片段头 8 字节**（`tail -c +9`）再 cat，保留用户已有记录。
  · **坑① gpt.ini Version 撞车**：gpsvc 拿 `gpt.ini Version` 与注册表 `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Group Policy\State\Machine\GPO-List\0\Version` 比对，**相同 = "无变化"直接跳过**（实测初装写 65537 恰好撞上镜像里的残留记录 → 脚本从不执行，事件日志 `no changes detected`、Extension-List 只有 `{00000000}`）。规则：**有文件 → 机器字 +1；没有 → 写 `2147418113`（0x7FFF0001，压过常见残留）**。另实测：**脚本每引导都会跑**（不需要反复改版本；失败重试天然可行）。
  · **坑② WOW64 文件系统重定向（PIT-082 家族，2026-10-06 实测）**：补写器是 **32 位** exe（x86/x64 目标通吃），在 64 位 Windows 上访问 `C:\Windows\System32\GroupPolicy\…` 被**重定向到 `SysWOW64`** → 组策略文件全读不到（清理静默跳过）、延迟删除删错对象；EA 目标若在 System32 下也会写错文件。修复：wmain 开头 `Wow64DisableWow64FsRedirection`（GetProcAddress 两步 cast）进程级关闭。
  · **坑③ CRT `ccs=UTF-8` 日志吞文本**：`_wfopen(…, L"a, ccs=UTF-8")` + `fputs` 写出的日志只剩 BOM + 每行 `\r\n` 的 UTF-8 形态（U+0A0D 乱码），全部文本丢失 —— 改用 `CreateFileW(FILE_APPEND_DATA)` + `WriteFile` 裸字节追加。
  · **清理语义**：SYSTEM 身份下可覆写/删除 gpt.ini/scripts.ini/Registry.pol（实测 ok）；正在执行的 cmd 与自身 exe 用 `MOVEFILE_DELAY_UNTIL_REBOOT`（下轮启动删；**注意**：延迟删除会在下轮启动删掉**同路径的新文件** —— 实验室重新部署后曾被上一轮的延迟删除误杀，真机不会遇到，但排查时别被绕）。
  · **验证手法（可复用）**：真实 Win10 的 VMDK → `qemu-img convert` → VHD（清 sparse 标志才能挂载）→ 开发机挂载离线注入 → QEMU/OVMF 启动（`-boot order=c`，ESP 放 `\EFI\BOOT\BOOTX64.EFI` 兜底）→ monitor `screendump`/`system_powerdown` → 重新挂载查 marker/日志。全自动、无 guest 凭据需求。
  · ✅ 2026-10-06（`0.6.38`；EA 链整体见 PIT-122）

### PIT-124

- PIT-124 **VSS 对挂载的离线 VHD 卷不可用（rc=89）→ 新增 `--no-snapshot` 冷备开关；junction 源被 wimlib 拒（rc=40）**（2026-10-06，`0.6.39`）：全链 e2e（PIT-122）需要备份一台**离线**的测试 VM 系统卷（挂载中的 VHD）：`backup --source F:/` → VSS precheck 正确拉起 vss/swprv，但快照创建失败 `rc=89 Unable to create a filesystem snapshot`（VSS 不支持这种卷）。两个绕道失败：① 建 junction（`D:\src` → `F:\`，非卷根 → 不触发 VSS）→ wimlib `rc=40 Expected a directory`（**拒 reparse point 作源**，带尾斜杠也一样）；② 无。正解：**新增 `backup --no-snapshot`**（`BackupRequest::noSnapshot`，`snapshot=(req.snapshot||volumeRoot)&&!noSnapshot`）—— PE / 离线卷 / VHD 卷上 VSS 不可用本来就是真实场景（冷备语义：卷静态、无需快照）。全链 e2e 用它完成。✅ 2026-10-06（`0.6.39`）

### PIT-125

- PIT-125 **发布体积两轮优化（55.8→40.6MB）：①exe 剥符号 ②initramfs 模块保持 `.ko.xz`（不再转 `.ko.gz`）**（2026-10-06，`0.6.40`/`0.6.41`）：
  · **① 剥符号（`-s`，`0.6.40`）**：全部 exe 链接加 `-s` —— x64 CLI -838KB / GUI -1.1MB / EA 补写器 -495KB（四套共 ~4MB；崩溃报告只用"模块+偏移"（PIT-084），不依赖符号表）。另 `package` 现在自动清 `dist\logs`（从 dist 直接运行 exe 产生的运行期日志/诊断包，属垃圾，不再随交付分发，-6.5MB）。
  · **② initramfs 模块格式（`0.6.41`）**：原实现是 `gzip(cpio(模块 .ko.gz))` —— **双重压缩**（模块早已 gz，外层 gzip 几乎无效）。内核配置实测 `CONFIG_MODULE_DECOMPRESS=y` + `CONFIG_MODULE_COMPRESS_XZ=y`（Debian 原生 .ko.xz，**内核原生解压**）→ 构建脚本改为**原样拷贝 `.ko.xz`**（不再 lzma 解压+重压），`modules.dep` 路径随之 `.xz`；`bootfiles/alpine/init` 的 3 处后缀处理（ldmod 的 builtin 判定、两个"全量 sweep" find 循环）加 `*.ko.xz`。**initramfs 19.5→15.2MB（-4.3MB），494 模块一个不少**；rootfs 里模块反而更小（内存占用下降），模块加载走内核原生解压（速度≈不变）。回归：QEMU drill 9/9 PASS（模块加载/ntfs3 挂载/wimlib apply/EA 投放全链）+ UEFI SB 链冒烟 PASS。
  · **未采用（用户 2026-10-06 裁定"影响兼容性/速度的一律不做"）**：a) "模块解压存储 + xz 外层"（可再省 ~7.8MB，但模块整棵进 RAM → **+50MB 内存**，侵蚀低内存兼容余量（PIT-113：512MB 才稳、192MB OOM），且外层解压更慢）；b) 裁小众模块（VDO/Ceph/DRBD/bcache 等，属兼容覆盖面，保留全部 494）。**可再议**：外层 gzip→xz（再省 ~0.5-1MB，需改产物名/引用，收益小）。
  · **当前体积账**：exe ~5.4MB（已剥）+ initramfs 15.2 + vmlinuz 11.6（SB 签名，不可动）+ sb 资产 3.6 + UCRT ~3（Win7 兼容）+ wimlib/grldr/补写器/skin/lang ~2 → **≈40.6MB**。

### PIT-126

- PIT-126 **论坛反馈核实（Win11 精简版装菜单失败）+ 两个真 bug 修复：①ESP 引导校验误伤精简系统 ②引导层失败后契约不回滚（按钮假「删除菜单」）**（2026-10-06，`0.6.42`）：
  · **反馈原文**：英文精简 Win11 + v0.6.3 装菜单报 `ESP boot verification failed (boot was NOT properly repaired; aborted before touching the target partition): …Fonts\*_boot.ttf`，且失败后按钮变成「删除菜单」。**逐字核实属实**（报错 = `Tr("ESP 引导校验未通过（未真正修好，已中止，未动目标分区）：")` 的英译 + 字体详情；且 0.6.3→0.6.41 代码顺序未变 → **当前版本同样会复现**）。
  · **根因①（误伤精简系统）**：`VerifyEspBcd` 把 bootres/引导字体/语言资源当**硬项**；精简镜像把 `C:\Windows\Boot\{Fonts,Resources}` 裁掉了 → bcdboot 无源可拷 → ESP 缺字体 → fail-closed 中止。而机器本来就能正常开机（字体只影响 bootmgr 菜单渲染；我们的 UEFI 菜单是**固件启动项**、不经 bootmgr）→ 纯误伤。**修复**：软项**源感知** —— `VerifyEspBcd(espRoot, srcWinDir, detail)`：ESP 缺 + **源也缺** = 精简镜像既成事实 → 放行 + detail 记 `[lite: source lacks …]`；ESP 缺 + **源有** = bcdboot 写一半 → 仍 fail-closed（保住 PIT-090 的保险丝语义）。判定抽成纯逻辑 `EspSoftItemVerdict`（uefi.h inline）+ 单测。副作用：此类机器的**跳过 bcdboot 判定**也随之通过 → 不再无谓重建 BCD（连带保住第三方启动项）。
  · **根因②（契约不回滚）**：`StageRestoreImpl` 里契约（目标根 `_zjresy` + exeDir `restore-task.conf`）在引导层**之前**写（菜单绑定/暂存任务都需要它）；引导层失败 return 时契约已落盘 → GUI `ReadMenuBinding` 误判"已安装"（按钮变「删除菜单」，用户实测）；且目标根留一份 `action=restore` 陈旧日志（将来进救援层可能被误执行）。**修复**：加 **RAII 回滚守卫**（`repairBoot` 时武装；引导层+单次启动段内**任何失败 return 都自动** `CleanupStrayContracts` + `DeleteMenuBinding` + 清数据盘副本；成功路径 `return 0` 前 disarm）。**注意**：只回滚契约，**不删**已部署的引导文件/启动项 —— 用户可能是在旧菜单基础上重装，失败时不能把旧菜单一起干掉。
  · **验证**：单测 **40 用例/263 断言**（新增 `esp_soft_item_verdict` 四态）+ `make check` 全绿 + CLI 编译零警告。🚧 待真机/精简系统复测（本机无精简 Win11；论坛用户升级后验证）。
  · **给受影响用户的处置**：① 失败是 **fail-closed 中止、未动目标分区**（系统没坏）；② 0.6.3 的"卡在删除菜单"是 PIT-121 的旧 bug（0.6.37 已修），升级即可；③ 手工清残留：删程序目录 `restore-task.conf|json` + 目标盘根 `_zjresy*.log`；④ 升级到 `0.6.42+` 后重装菜单应直接成功。

### PIT-127

- PIT-127 **跨固件还原实测（GPT/UEFI ↔ MBR/BIOS 双向均通过）；顺带证实两条设计关键点（ESP 子镜像不含 BCD / bootfix 恒为 BIOS 味道）**（2026-10-06，`0.6.42` 代码验证、无代码改动）：
  · **背景**：用户提问核实"备份从 GPT 恢复到 BIOS、或反之，会不会出问题（含镜像有无 ESP 两种情况）"→ 实验室两轮**真启动**验证（QEMU 直启内核跑救援 + 真 Windows 首启）：
    ① **GPT/UEFI 源（镜像含 ESP 子镜像）→ MBR/BIOS 目标**：救援层 `apply rc=0` → bootfix（BIOS 味道）→ PBR 引导区 → **`WARN: task has esp subimage (idx 3) but no ESP partition found (skip)`**（带 ESP 的镜像到无 ESP 机器 = 正确跳过、不中断）→ **SeaBIOS 启动到桌面 ✓**（截图 `<lab>\e2e2\shot-bios1/2.png`）。
    ② **MBR/BIOS 源（无 ESP）→ GPT/UEFI 目标**：`apply rc=0` → `UEFI/GPT: ESP untouched, skip PBR` → EA 投放 → 用 `bcdboot /f UEFI /nofirmwaresync` 补 ESP（等价真机暂存侧 bcdboot；`/nofirmwaresync` 避免动本机 NVRAM）→ **OVMF 启动到桌面 ✓**（`shot-uefi3/4.png`）。
  · **两条设计关键点（本次实测顺带证实）**：a) **ESP 子镜像不含 BCD**（捕获时排除 `\EFI\Microsoft\Boot\BCD`；当初的原因是被运行中的 Windows 锁住，见 ESP 备份条目）→ 还原到 UEFI 目标时**不会覆盖目标机自己的 BCD**（目标 BCD 指向同一分区 → 恢复后照样启动，这是"UEFI 目标机跳过 bcdboot"安全的前提）；b) `PrepareBootFixFiles` **恒为 BIOS 味道**（无 `C:\Boot\BCD` 时先 `bcdboot /f BIOS` 生成，再改写 `{default}` 为 `device boot` + `path \Windows\system32\winload.exe`）→ 目标无论固件类型都可用（UEFI 下仅作摆设，UEFI 引导走 ESP）。
  · **边界/注意**：a) UEFI 目标若 **ESP 全空**（新盘）→ 依赖**暂存侧 bcdboot** 先写好（0.6.35+ "BCD 有效即跳过"，无效时会重建）；b) UEFI 目标 ESP 的 BCD 若指向**别的分区**（多系统 / 还原到非原系统分区）→ 跳过逻辑不比较目标分区，可能启动旧系统（边缘场景，暂记录）；c) 实验室 OVMF 需**空 NVRAM** 或 `\EFI\BOOT\BOOTX64.EFI` 兜底（旧 vars 会优先走失效条目 → PXE）；真机 NVRAM 里有目标机自己的条目，不受影响。
  · 实验素材：`<lab>\e2e2\`（实验室目录，不入库）。✅ 2026-10-06（`0.6.42`）

### PIT-128

- PIT-128 **目标盘误判隐患：Linux 给两块 IDE 盘的 sd 字母会变 + 多盘首分区都在 1MiB → 只比 offset 会把镜像盘当目标盘；修复 = offset 匹配必须同时校验 size（双端）**（2026-10-06，`0.6.43`，实验室意外触发）：
  · **怎么发现的**：批量测试（docs/19 批次 A，Win10 精简镜像）时漏写目标盘 `_zjresy` 日志（实验室失误），救援层退到"按 offset 匹配"；恰好那次两块 IDE 盘（目标 40GB / 镜像 16GB，**首分区都在 1MiB**）的 **sd 字母与上次相反**（libata 异步探测顺序不保证，实测同一 QEMU 参数两次启动 sda/sdb 对调）→ 镜像盘被选成"目标"→ 镜像扫描又跳过"目标"→ 在另一块盘上找不到镜像 → 安全中止（**镜像扫描先于 mkntfs，没动任何盘** —— 顺序救了场）。
  · **修复（双端）**：① `zjrestore-lite.sh`：新增 `dev_size()`（sysfs size×512）；`target_by_offset` 与"日志候选"校验改为 **offset 必须匹配 +（双方都有 size 时）size 也必须匹配**，不符即 `target-reject … size-mismatch` 跳过；② `bootfiles/alpine/init` 的 `scan_for_log` 同样加 `target_part_size` 校验（防旧日志把别的同 offset 分区指成目标）。
  · **直接验证**：确定性布局（镜像盘挂 NVMe=枚举在前、目标挂 IDE、无目标日志）→ 日志出现 `target-reject dev=/dev/nvme0n1p1 offset-ok size-mismatch 17177772032/42948624384` → 正确选中 40GB 目标 → `found image`（在镜像盘）→ `apply rc=0` → RESTORE DONE ✓；drill 回归 PASS（正常流程不受影响）。
  · **教训**：任何"按位置猜设备"的逻辑都必须**多字段校验**（offset+size，能加 serial 更好）；`/dev/sdX` 字母不可靠（跨启动可变，勿持久化）。

### PIT-129

- PIT-129 **"不支持的系统镜像"主动拒绝（I-1 落地）：镜像必须有 `\Windows\system32\winload.exe`，否则写契约/格式化前拒绝**（2026-10-06，`0.6.44`；docs/19 批次 G 实测定稿）：
  · **为什么**：老系统镜像（XP/2003/2000/98）与"安装源类"镜像（实测某 XP esd 连 ntoskrnl 都没有）还原后会"**格式化完才发现起不来**"（Win7+ 才有 winload.exe/BCD 体系）。批次 G 实测：XP x86/x64、2000、98、2003 全部无 winload；Win7 对照组有 ✓。
  · **实现（双端）**：① Windows 侧 `StageRestoreImpl` 步骤 0.7：新增 `WimEngine::ImagePathExists()`（`wimlib_iterate_dir_tree`，只读元数据）探 `\Windows\System32\winload.exe`，缺失即拒绝（rc=4，未动任何东西）；② 救援层 `zjrestore-lite.sh` 在找到镜像后、**动目标分区前**同款检查（`wimlib-imagex extract` 单文件探测，带 timeout；缺 → `STEP=unsupported-image` 退出）。
  · **四个实测坑（都踩了）**：a) **`wimlib-imagex dir` 在 solid ESD 上会卡死**（首次调用 rc=2、二次调用挂到超时）→ 弃用 `dir`，改单文件 `extract` 探测；b) **Linux 侧 wimlib 路径匹配区分大小写**（真实镜像里是 `System32` 大写 S）→ 必须**双大小写各探一次**（Windows 侧不区分）；c) **`set -u` 下 `$WIMLIB` 未定义**：变量原本在 apply 段才赋值、预检在前 → 脚本 rc=2 直接退出（已把解析提前）；d) 初版用 ntoskrnl 当"系统镜像"门槛是 **fail-open 漏洞**（安装源类镜像没有 ntoskrnl → 被当"非系统镜像"放行）→ 改为单一硬规则"必须 winload.exe"。
  · **演练盘适配**：`drill-src` 补 `Windows\system32\winload.exe`（dummy），test.wim 重建后 drill 照常 PASS（演练素材本来就不是真系统镜像）。
  · **验证**：XP esd 负例（无 mkntfs/无 apply + 明确报错）✓；Win7 x86 正例（precheck ok → apply rc=0 → RESTORE DONE）✓；drill 回归 PASS ✓。

### PIT-130

- PIT-130 **`repair-boot` 机器侧回归完成 + 实验室"重启类"实验的休眠陷阱 + lite 镜像 GPO 行为修正**（2026-10-07；docs/15 §13 全过程）：
  · **回归结果**（在还原后的真实 Windows 里自动执行 `SysRecover.exe repair-boot`，0.6.44）：**UEFI 正常 ESP** ✅（`target=C: disk=0 part=4` → `bcdboot (UEFI) rc=0` → `displayorder {default}` → 校验通过 rc=0）；**UEFI + ESP 类型 GUID 被改成 Basic Data**（模拟 DiskGenius 重建分区）✅（走 `FindEspPartitionFallback`：`[WARN] ESP partition type is not EFI System; used fallback` → rc=0）；**BIOS/MBR** ✅（`bcdboot (BIOS) rc=0`）。docs/15 §12.3 两项待做全部关闭。
  · **⚠️ 实验室休眠陷阱（做"重启类"实验必读）**：5509 lite 镜像上 `system_powerdown` 触发的是**休眠**（写 6.4GB `hiberfil.sys`）→ 下一次"启动"实为**恢复会话**：**不执行任何启动脚本、无需登录**（本轮曾因此得出"GPO 脚本不执行"的**假阴性**）。**做重启类实验前必须**：删 `<系统盘>\hiberfil.sys` + 置 `HiberbootEnabled=0`（SYSTEM hive `ControlSet001\Control\Session Manager\Power`），并确认下次是真冷启动。
  · **GPO 钩子在 lite 上"能用但时机存疑"**：Scripts CSE 实测**能执行**（BIOS 目标 `gpupdate /force` 后 / UEFI 目标冷启动**开机期**，均 `nt authority\system`，脚本 rc=0）；但 `Registry.pol` 的 `RunStartupScriptSync`/`SyncForegroundPolicy` 在 lite 上**未落注册表**（标准镜像上生效、EA e2e 曾实测登录前 `shell_started=no`）→ 脚本可能是**异步执行（登录后 ~1 分钟）而非登录前同步**。**影响 EA 修复链**（首启补写要在 explorer 前跑完）——**2026-10-07 已定案（见 PIT-131）**：根因是 gpt.ini 版本低字碰撞导致 gpsvc 静默跳过处理；修复后实测**登录前执行**（`shell_started=no`），无需备用钩子。
  · 次要：ESP 被 `mountvol` 挂上盘符时 `LogBaseDir()` 会把它当数据盘，把 `logs\` 写进 ESP 的 `ZJRESTORE\logs\`（轻微污染，记录备查）；`cmd.exe /c x.cmd` 注册成服务（ImagePath）在本机**未触发**（SCM 日志被 lite 精简，原因未查）——**RunOnce 通道稳定可用**。
  · 测试环境：实验室 QEMU（OVMF/SeaBIOS）+ 5509 备份还原出的真实 Windows；钩子（本地 GPO 四件套 + RunOnce）已按 PIT-123 配方部署，测后已清理（`C:\zjtest\` 保留作证据）。

### PIT-131

- PIT-131 **EA 首启补写器在"带组策略残留"的镜像上静默不执行——根因 gpt.ini 版本低字碰撞；修复 = 时间基版本**（2026-10-07，`0.6.45`；5509 lite 镜像全链实测定位，客户 EA 修复的关键加固）：
  · **现象**：5509 系统备份（Windows 侧 `backup --no-snapshot`，EA 采集 255 文件 → ZJEA 子镜像）经救援层还原后，**首启补写器不执行**（无 ea-apply.log、EA 未写回、清理未发生）；多次重启、`gpupdate /force` 均不触发；而同一镜像经 ESD 还原的另一台机器上 `gpupdate` 后能跑——一度极难定位（曾误判为"lite 镜像 GPO 不工作"）。
  · **根因（对比实验定死）**：镜像注册表里遗留 `...\Group Policy\State\Machine\GPO-List\0\Version = 0x00010001`（本地组策略"已处理"的常见残留），而部署的 gpt.ini 写 `0x7FFF0001` —— gpsvc 的变更检测**只比较低 16 位**（用户版本字）：两个低字都是 `0x0001` → 判"无变化"→ 跳过整个处理 → **Scripts CSE 永不执行**（PIT-123 的 65537 碰撞是同一机制的另一种撞法）。实验：State 清零（低字 0）→ 立即处理并执行 ✓；gpt.ini 改 `0x7FFF7FFF`（低字 0x7FFF）而 State 保持 0x10001 → 同样执行 ✓（且 State 被记录为新版本）。
  · **修复**：`zjrestore-lite.sh` 的 EA gpt.ini **新版本改用时间基低字**：`2147418112 + (date +%s) % 32768`（= `0x7FFF0000 | 低字`；合并分支的 `VER+1` 天然低字不同，保留）——保证与任何残留/上次部署的版本都不同，杜绝静默跳过。
  · **验证**：① 实验室全链（5509 lite 备份 → 救援还原 → 首启）：`shell_started=no (pre-logon)`、`ok=255 missing=0 failed=0`、清理全项完成、EA（`ZJTEST`）实测写回、State 正确记录新版本 ✓；② `make check` 40 用例/263 断言 + check-docs/i18n 全绿；③ QEMU 演练 PASS（`ea: gpt.ini created (version=2147427948)` → `RESTORE DONE`）；④ `make package` 双架构。
  · **教训**：凡"版本号当变更检测"的机制，要假设比较可能**只取部分位**——固定魔数（0x7FFF0001）会撞上常见残留（0x10001）；**时间基/随机基最稳**。另（复测再踩）：补写器的延迟自删（`MOVEFILE_DELAY_UNTIL_REBOOT`）会删掉**下次启动前重新部署的同路径新文件**（PIT-123 已记）——重放实验必须先把 exe/cmd 补回并注意其注册已消费。

### PIT-132

- PIT-132 **"悬空引用"根治：注册表指向备份排除目录（Temp 等）的组件还原后必坏 —— 备份扫描 + 首启自动清理**（2026-10-07，`0.6.46`；客户右键卡死的最终修复）：
  · **背景**：客户"用我们的软件还原后右键打圈/卡死"在 EA 修复（PIT-131）后依旧（原话："一点右键就打圈；重启资源管理器后左键能用、右键不行"）。定因：**右键菜单处理器损坏** —— 豆包便携版把扩展 DLL 注册在 `HKCR\*\shellex\ContextMenuHandlers\{A5AF131F-…}` → `…\AppData\Local\Temp\DoubaoPortableTemp\…\shellext.dll`；**我们的备份排除 `\Users\*\AppData\Local\Temp*`（`exclude.cpp:35`）→ DLL 不进镜像，注册项却随注册表还原** → "有注册、无 DLL"的坏处理器 → 每次右键加载它 → 卡死。易数/DiskGenius 是**扇区/整分区级**还原（Temp 原样保留）→ 不卡 —— "只有我们卡"的差异吻合。等待链（UI 线程等 shell 工作线程）+ explorer 转储（shell32→combase 处理器加载路径阻塞）佐证。
  · **不止右键**：启动项（Run/RunOnce）指向 Temp、COM 注册、服务、计划任务等"注册表引用指向被排除路径"都会在还原后悬空（启动项→登录报错弹窗；COM→应用异常）。
  · **实现（通用扫描器 + 保守自愈，`src/common/refscan.{h,cpp}`）**：① 扫描 shell 扩展（7 个右键根 + 覆盖图标 + ShellExecuteHooks；**HKLM 双 WOW64 视图**——x86 补写器必须 `KEY_WOW64_64KEY` 才看得到 64 位视图，PIT-082 家族）+ 启动项（Run/RunOnce：机器双视图 + 用户 hive——备份扫已加载的 HKU，首启从磁盘加载 `Users\*\NTUSER.DAT`）；② 判定 = 路径片段落在易失目录（AppData\Local\Temp / Windows\Temp / CbsTemp / winsxs\InstallTemp / INetCache）；③ **备份时**（`ops.cpp`）：统计"易失目录引用"（`missingNow` / `at-risk` 两状态都算）→ >0 时并入 ZJEA 修复包（无 EA 也可只有补写器）；④ **首启**（补写器、登录前 SYSTEM）：`CleanAllDanglingRefs` **只删"易失目录 + 文件已缺失"**的项（存在的不动），删前落 `refs-backup.txt`（可恢复）、结果落 `refs-result.txt`；⑤ 救援脚本载荷校验放宽（**补写器必需、eapack 可选**）+ 启动 cmd 双模式（有包 `--pack`，无包直接跑）；⑥ `diag` 输出 `volatile-ref: N (missing now=…; at-risk=…)` 供支持包取证。
  · **验证**：① 单测 `refscan_volatile_path` / `refscan_extract_exe`（`make check` **42 用例/282 断言**）；② **机内全链 e2e**（注入假引用 → utarget 机内 VSS 备份：`ref scan: 2 volatile-dir reference(s) (0 missing, 2 at-risk)` → `EA/fix pack appended (ea=255 refs=2)` → 救援还原（`applier=yes`、时间基 gpt 版本）→ 首启：**EA 255/255 + `refs: found=2 cleaned=2 failed=0 atRisk=0`**；离线核验两条注册项消失、`refs-backup.txt` 在案、EA 抽查在）；③ drill PASS；④ `make package` 双架构。
  · 备注：用户 hive 加载尽力而为（实测 `admin` 的 NTUSER.DAT 加载 rc=32 共享冲突 → 优雅跳过并记录；主清理在机器级）；`[ExclusionException]` 路线被否（**wimlib 不扫描被排除目录的子树**，例外救不回目录内的文件，官方论坛/MS WIMGAPI 同款行为）。
  · **扩展（`0.6.47`，同 PIT-132）**：扫描范围再加三类 —— ① **COM 注册**（`HKLM\SOFTWARE\Classes\CLSID\*\InprocServer32|LocalServer32` 默认值，**双 WOW64 视图**；删除 = 删整棵服务器子键）；② **服务 ImagePath**（`HKLM\SYSTEM\CurrentControlSet\Services\*`，不分视图单遍；删除 = 删 ImagePath 值）；③ **计划任务**（`System32\Tasks` 下 XML 的 `<Command>`，含子目录深度 ≤3；删除 = `schtasks /Delete /TN <相对名> /F`，登录前实测可用）。**e2e（6 类引用一次全测）**：注入 shell 扩展/Run/COM×2/服务/任务 → 备份 `ref scan: 6 (0 missing, 6 at-risk)` → `EA/fix pack (ea=255 refs=6)` → 救援还原 → 首启 `refs: found=6 cleaned=6 failed=0` + 离线核验 5 处注册表全消失、任务文件已删 ✓。**扫描耗时**：真机 `diag` 实测全扫描 **~1s**（含 ~2 万 CLSID ×2 视图）；QEMU TCG 下约 23s（一次性、登录前）。单测加 `refscan_task_commands`（`make check` 43 用例/286 断言）。

### PIT-133

- PIT-133 **4Kn（4K 原生扇区）目标盘实测：还原管线全通；4Kn+BIOS 组合微软不支持 → 救援打 WARN 不阻断；顺带补上 GUI 加载镜像的日志**（2026-10-07，`0.6.48`）：
  · **实测（QEMU 造 `logical_block_size=4096,physical_block_size=4096` 目标盘，完整救援还原）**：`offset+size` 校验（按字节）✓、`mkntfs` 按 4096B 逻辑扇区格式化 ✓、wimlib block apply ✓、bootfix/PBR 写入 ✓、`RESTORE DONE` ✓ —— **原先担心的"512 假设"实际都不需要改**（脚本的 `dd` 按字节偏移；`--partition-start` 由 mkntfs 按 LBA 自算；引导区 blob 是文件内容与扇区大小无关）。**结论：管线本身对 4Kn 透明。**
  · **⚠️ 但 4Kn 必须 UEFI 引导**（微软要求；4Kn+BIOS 组合不受支持）→ `zjrestore-lite.sh` 对 4Kn 目标打 `WARN: target is 4Kn (logical sector=…): Windows does not support BIOS boot from 4Kn; on a BIOS/MBR machine the restored system will not boot (4Kn requires UEFI/GPT)`（**不阻断**——数据已按正确语义写完，引导问题属组合限制）。
  · **附带（同批发现并修复）**：GUI `LoadWimImages` **成功路径原先既不改状态栏也不写日志** → 支持排查/自动化都看不出"加载过哪个镜像"（2026-10-07 GUI 静默全链实测时踩到：日志停在启动块，误判"未加载"）→ 补 `GUI image loaded: <path> (N subimage(s), first idx=M)` 一行（`src/gui/main_form.cpp`）。
  · **验证**：4Kn QEMU 全链还原 + **GUI 静默模式全链**（无弹窗→暂存 `pending-restore.txt`→自动重启→救援 `result=OK target=/dev/sda4 image=E:\images\refs2-test.wim`（ESP 黑匣子）→首启 EA 255/255 + refs 6/6 `shell_started=no`→新系统桌面）；`make check` 43 用例/286 断言 + i18n 401 keys；`make package` 双架构。实验室 GUI 自动化配方见 `docs/19` §8。

### PIT-134

- PIT-134 **悬空引用清理在客户机上失效的真根因：WOW64 视图 + 跨 hive 组合（x86 补写器 view=0 只看 32 位视图；处理器与 CLSID 分处不同 hive）**（2026-10-07，`0.6.49`；客户 20261007 支持包 + 实验室两轮复现）：
  · **现象**：客户 0.6.47 还原成功（救援全绿 `result=OK`、系统正常进桌面）后，`diag` 仍报 `shellext ctx\{A5AF131F-…} … *** MISSING ***`（豆包失效扩展），右键卡死依旧——PIT-132 的清理"跑过但没动它"。
  · **实验室复现（关键一步）**：VM 注入"跨 hive + 64 位视图"组合（处理器 `HKLM\SOFTWARE\Classes\*\shellex\ContextMenuHandlers\{…}`、CLSID 的 InprocServer32 在 `HKCU\Software\Classes\CLSID\{…}`，64 位写入）→ 补写器 **`found=0`**（一个都没找到）。逐层定位两个真 bug：
    ① **用户根扫描用 view=0**：`HKCU\Software\Classes` 及类名/CLSID 树**受 WOW64 重定向**——x86 补写器的 view=0 只看到 32 位视图（Wow6432Node），**64 位程序注册的壳扩展/COM 在用户侧全部漏掉**。机器侧本来就双视图（`ScanMachineView` ×2），所以 e2e（引用全注入 HKLM）从未暴露。
    ② **`ResolveClsid` 只在本 hive 解析**：处理器在 HKLM、CLSID 在 HKCU（便携软件常见）时，机器扫描在 HKLM 找不到 CLSID、用户扫描又找不到处理器 → **双双漏掉**；而 `diag` 用 HKCR 合并视图能看到 → "诊断标 MISSING、清理不动它"的怪象。
  · **修复**：`ScanUserRoot` 类相关部分 64/32 双视图各扫一遍（共享部分同扫，清理端按 display 去重）；`ResolveClsid` 跨 hive + 各视图（本根→HKLM 双视图→HKCU→所有已加载用户 hive，每处都试 64/32/0）。⚠️ 中途曾漏改 `ResolveClsid` 的 HKCU/HKU 视图（仍 view=0）→ 测试第二轮 `found=1` 只清掉用户 CLSID、HKLM 处理器仍漏——**修完再验才过**。`0.6.50` 追加保险：预登录磁盘 hive（`ZJREF_n`，非 SID 名）**显式再扫字面 `Software\Classes\Wow6432Node\`**（view=64 读字面路径）——WOW64 重定向是否作用于手动挂载 hive 无定论，显式扫一遍保证 32 位程序的注册也不漏（SID 路径由 view=32 遍覆盖、不重复）。另 `0.6.50` 把 `--lang` 写进 CLI 总览帮助（用户反馈 help 没写）。
  · **验证（实验室 VM 两轮）**：注入组合后跑新补写器 → `refs: removed HKLM\…\ContextMenuHandlers\{…}` + `refs: removed HKU\S-1-5-…\…\CLSID\{…}\InprocServer32`、`found=2 cleaned=2 failed=0`、两条 `reg query` 均"找不到" ✓。
  · **⚠️ 生效前提（对客户）**：补写器是**备份时打进 ZJEA 子镜像**的——**老镜像里还是旧补写器**。① **立即补救**：把 0.6.49 的 `zj-ea-apply.exe` 拷给客户管理员运行一次（无包模式只清悬空引用）→ 右键应恢复；② **根治**：用 0.6.49 **重新备份**再还原。⚠️ 测试时注意：补写器自清理的"延迟删除"会在下一轮启动删掉**同路径的新文件**（PIT-123 老坑，本轮又踩：重部署同名 exe 被删 → 换名 `zj-ea-apply2/3.exe` 才跑起来）。
  · **附带修复**：`selfdiag` 的部署痕迹收集把 `ZJRESTORE\ea\` 加进收集子树（此前 applier 的日志/结果不在支持包里——这次排查因此少了关键证据）。`make check` 全绿；`make package` 双架构；分析全文见 `docs/16` §10。

### PIT-135

- PIT-135 **客户 20261008 两问题：①非微软重解析点令 Linux apply 必 rc=58 中止（备份端自动排除）②黑匣子 .out 三件套不在清理清单（失败后残留所有分区根）**（2026-10-08，`0.6.51`）：
  · **① 三盘机（Dell ChengMing 3900，= PIT-112 同机）**：暂存还原 → 救援 `mkntfs` 后 apply **8 秒**即 `rc=58 Failed to set reparse data on "/Windows/ServiceProfiles/LocalService/AppData/Local/Intel/ipfsrv/ipfsrv.dptf": Invalid argument` → 整个还原中止、**目标 C: 只剩半成品（系统被格式化后无法启动**，客户随后在 PE 里导出支持包）。根因：**Linux 侧 wimlib（NTFS-3G 块模式）无法设置"非微软（ISV 自定义）重解析点"**——wimlib NEWS 原文 *"remains broken in NTFS-3G mode due to a libntfs-3g bug"*；wimlib 论坛同文件（`ipfsrv.dptf`）两例**无解**；**Windows 侧（PE 就地还原）无此问题**。**修复（备份端预防，`0.6.51`）**：`ea::FindNonMsReparse`（快速遍历、只读 `dwReserved0`；判定 = tag bit31 为 0）→ `EnsureExclusionConfig(source, &badRep)` 追加排除 + WARN 日志；单测 `ea_non_ms_reparse_tag`。**对客户的处置**：**PE 里"就地还原"**（Windows 侧 wimlib 能设该 reparse）立即恢复；或用 0.6.51 **重新备份**后暂存还原。**回滚 0.6.3 无用**（同一 wimlib 限制，非回归）。**2026-10-19 补课**：同机用旧镜像再次 rc=58 → 见 **PIT-157**（谓词与上游逐行同构 + 就地还原补写 + block 暂存预检，F1~F6）。
  · **② "所有分区根垃圾文件"**：失败还原的黑匣子按设计多面落盘（PIT-111），但 `CleanupStrayLogs` 只删三件套 + `zjrestore-boot.log`——**`ZJRESTORE-apply.out / mkntfs.out / esp.out` 不在清单** → 客户导出支持包后仍逐盘残留（实测截图 + RAR：每盘根恰好只剩这两个文件）→ **修复：清理清单与收集清单都加 .out 三件套**。
  · **③ 同机问题 1 的"三个 UEFI 启动项"**：截图三个 `UEFI: Windows Boot Manager` 均为客户自己装的 PE/易数条目（**我们的条目描述是 `SysRecover`、直指内核不走 bootmgfw，不在其中**）；**我们从不改固件默认（BootOrder[0]）**——条目挂 BootOrder 末尾、单次启动走 BootNext；"每次还原完默认第 1 项"是固件自身顺序。桌面快捷变少 = 用旧镜像的必然结果（镜像之后装的软件不在）。
  · **④ "用旧镜像还原后右键仍卡"**：镜像里打包的是**旧版补写器**（PIT-134 的 WOW64 视图 bug）→ 修复对老镜像不生效；处置：新补写器跑一次 或 0.6.50+ 重新备份。

### PIT-136

- PIT-136 **（空号补记，2026-10-28）**：早期 8 处注释写 `// PIT-136` 指的是 **reppack.dat 首启写回**（重解析点/EAP 修复包在首启补写时的清理顺序）。该内容后来并入 `PIT-135`（重解析点/EA 投放大类）与 `PIT-157`（F1~F6 双端加固 + repack 回放）—— **以那两条为准**；此处保留编号只为消除全仓悬空引用。

### PIT-137

- PIT-137 **黑匣子归并：Windows 每次启动"收完即清"，救援层不再每分区乱放**（2026-10-08，`0.6.53`，用户规格）：
  · **背景**：失败还原的黑匣子按 PIT-111 设计"多面落盘"（每个可挂载分区根都写）——客户逐盘可见的"垃圾文件"观感差；且清理只在"日志导出成功后"发生。
  · **① Windows 侧（每次运行收完即清）**：CLI/GUI 启动路径在 `CollectDeployArtifacts`（已收集 `ZJRESTORE-logs` 兜底窝 + ESP 日志 + 各盘根黑匣子）之后调用 `CleanupStrayLogs()` —— **黑匣子进 `logs\collected\`，各盘根散件立即清掉**。安全边界不变：**契约 `restore-task.*`/`_zjresy*` 与引导文件（grldr/menu.lst/bootfix）绝不动**；只清固定盘；活动日志根（LogBaseDir）自身不删；暂存待还原的流程不受影响（那些文件在暂存时本就不存在）。
  · **② 救援层（只写固定三处）**：失败路径与 `zz-blackbox` 模式**不再 `bb_sweep` 全盘扫**，改为 `bb_home_drop`（兜底窝 `<数据盘>\ZJRESTORE-logs\`，没有就现认领一个非目标分区；控制台文本仍收进日志）+ `bb_esp_drop`（UEFI 的 `\EFI\ZJRESTORE\logs\`，格式化也幸存）；软件目录由 `persist_log` 写。`bb_sweep` 退役（函数保留不调用）。单分区极端场景的保护面与原来等价（目标盘本来就会被格式化）。
  · **验证**：救援脚本 `bash -n` 语法过；`make check` 45 用例/304 断言 + check-docs/i18n 全绿；initramfs 重建（494 模块）。🚧 待 QEMU 失败演练回归（造坏 conf → 失败 → 检查只落"兜底窝+ESP"、各盘根干净）。

### PIT-138

- PIT-138 **A 引擎（swap）+ esp_index 必崩：`WIMLIB`/`IMG_FILE` 只在 B 引擎分支里赋值 → `set -u` rc=2 中断在 ESP 段；另两处 `set -u` 让 init 的兜底黑匣子也写不出日志**（2026-10-12 现场日志定位，`0.7.27`）：用户报"3 个用户报错，还有 1 个在 Linux 下无法继续操作"。拿到日志包 `SysRecover-logs-20261012-220344.zip` + 现场照片后逐层对齐：
  · **现场证据**：回执 `ZJRESTORE-status.txt` = `result=UNKNOWN step=esp build=0.7.13`；`zjrestore-debug.log` 停在 `restore esp subimage idx=2 -> /dev/sda1`（**没有** `esp restored`、**没有** `RESTORE DONE`、**没有** `result=OK`）；init 打 `restore script returned (rc=2)` → `blackbox sweep by init (reason=interrupted)` → `zjrestore-lite start … engine=swap` → `blackbox sweep start` → **之后零落盘** → `WARN: no writable surface found for logs` → `RESULT: restore FAILED (rc=2)` → `entering rescue shell`。
  · **根因①（主因）**：`WIMLIB="$(command -v wimlib-imagex …)"` 原本**只**出现在两处，都在 `if [ "$ENGINE" != "swap" ]` 的分支里（镜像预检段 + B 引擎 apply 段）。A 引擎两个都不走 → `"$WIMLIB" apply "$IMG_FILE" "$ESP_INDEX" "$ESPM"`（ESP 段）展开**未定义变量** → `set -u` 直接退出 **rc=2**，正好死在 `restore esp subimage` 那一行之后。**讽刺点**：脚本自己的注释早写明"set -u 下引用未定义变量会直接 rc=2 退出（2026-10-06 实测踩到）"，但当时只修了预检段，**apply 段没修**。时序上 swap 已 `rc=0`（新系统在位）、bootfix 已跑（`WARN: no bootfix files`），所以用户看到"好像还原完了，其实卡在 shell"。
  · **根因②**：`IMG_FILE=""` 与整个镜像扫描/预检段同在一个 `if [ "$ENGINE" != "swap" ]` 里 → swap 下 `IMG_FILE` **恒为空**，即使修了①，ESP/EA 子镜像也拿不到镜像路径（`wimlib apply "" …` 会以另一种方式失败）。
  · **根因③**：`bb_home_drop()` 里 `[ "$_d" = "$TARGET_DEV" ]` 裸引用，而 `zz-blackbox` 分支（init 的兜底黑匣子）跑在 **`TARGET_DEV="${1:-}"` 之前** → 同样 `set -u` rc=2 → init 的兜底落盘**一次都没执行**（正是照片里 `blackbox sweep start` 后面什么都没有的原因）。`find_esp_dev()` 里两处裸 `$TARGET_DEV` 同病（`bb_esp_drop` 也走它）。
  · **根因④（次级）**：`bb_home_drop` 第二次进入时 `/tmp/zj_home2` 还挂着（被 `is_mounted` 跳过），其它分区又被占 → 即使③修好也可能"没有日志面"。
  · **修复（全部在 `bootfiles/zjrestore-lite.sh`）**：① `WIMLIB` 解析**移到镜像扫描段之前无条件执行**（删除 B 引擎分支里的重复赋值，两处留注释指向）；② `scan_image()` 函数定义移出 `if`，新增独立的"**A 引擎带子镜像：补一次镜像扫描**"段 —— 找得到照常解子镜像，找不到**只 WARN 并清空 `ESP_INDEX`/`EA_INDEX`，不中断主 swap**（与 ESP 段既有的"失败只报错不中断"语义一致）；③ 脚本开头 `TARGET_DEV=""`（覆盖 `bb_home_drop`/`find_esp_dev` 全部裸引用）+ `bb_home_drop` 改 `${TARGET_DEV:-}` 并写明原因；④ `bb_home_drop` 开头先复用已挂载的 `/tmp/zj_home2`；⑤ `EA_INDEX` 解析从脚本尾部上移到与 `ESP_INDEX` 并列（否则②的补扫段判不到它，且 `set -u` 会踩）。
  · **验证**：`bash -n` 过（本机无 sh，用 `C:\Program Files\Git\bin\bash.exe -n`）；`tools/build-debian-rescue.py` 重建 initramfs（494 模块 / 15.9MB gz / **stamp 0.7.27**）；`make check` + `make package`。✅ **QEMU A 引擎 + esp_index 端到端演练 13/13 PASS**（2026-10-13）：`mk-drill.py` 新增 `ZJ_DRILL_ENGINE=swap`（契约写 `engine=swap`、首启菜单带 `zjsetup=1` → init 跑 `setup-swap.sh` 把 sda2 做成"旧系统 + `~new/` + `.zj-done`"后自动摘参），新增 `tools/vmtest/run-swap-drill.ps1`（两次启动：setup → restore）。关键回放：`found image: /tmp/zj_img/images/test.wim (dev=/dev/sda3)`（**补扫段生效，swap 下 IMG_FILE 不再为空**）→ `swap rc=0` → `restore esp subimage idx=2 -> /dev/sda1` → **`esp restored (rc=0)`**（修前死在这一步 rc=2）→ `ea subimage: index=3` → `RESTORE DONE: /dev/sda2` → 无 `restore script returned (rc=2)`。
  · **⚠️ 排查手法（可复用）**：该文件在 PowerShell 5.1 的 `Get-Content` 下**行数会少 ~180 行**（rg 1508 行 vs PS 1268 行），用 PS 行号切片会全部错位；**必须以 `rg -n` / Read 工具的行号为准**。另外本仓 `rg` 对 `tools\vmtest\base\alpine-stage` 下文件会报 os error 123，管道里混入 `|` 会报"文件名、目录名或卷标语法不正确"（是 PS 把 `rg` 输出当路径解析），要先把结果收进变量再过滤。

### PIT-139

- PIT-139 **机房测试四问题：③「返回旧系统」报错 = `UndoSwapLeftover` 根下同名全判 conflict 却 `return conflict==0` → 恒 false 且零字节未还原（修 = 方案 C「重启进救援层反向交换」）；顺带 ① 根目录 `LetRecovery_Scripts` 是第三方镜像自带（只加只读 diag 行）**（2026-10-13，`0.7.2x`）：用户报机房 4 个问题（另两个见 ②④，本轮与 ② 的 `main_win`/`main.cpp` 改动同批）。逐个定性后只动 ③。
  · **问题 3 根因（`src/app/swapleft.cpp::UndoSwapLeftover`）**：遍历 `~old\*` 搬回根时，**根下已有同名一律 `++conflict` 并 `continue`**（一个字节都不搬），返回值却是 `conflict == 0 && failed == 0` → keepOld 场景（交换成功、根下仍是**当前新系统**，同名几乎必然命中）**每次都是 false**，而实际还原量为 0 → 客户看到"报错"，且树一个没动。更深一层是**语义错配**：docs/23 §5 的 AskUndo 决策表针对"交换中断、根已空缺"，keepOld 是"交换成功"。且在 Windows 里改名正在运行的系统树本就不可回滚（explorer/注册表句柄占用）。
  · **方案 C（用户拍板）：Windows 只暂存，目录反向交换一律在 Linux 救援层完成** —— 新增契约取值 `action=returnold`（**只是既有 `action` 键的第二个取值 → 增量键，不升 `contract_version`**；旧救援层不认 `action` 照常走 swap → `zjswap.sh` 查不到 `~new/.zj-done` → `exit 2` fail-closed，绝不 mkntfs）。链路：`StageReturnOld` 写契约（`action=returnold` + `engine=swap` + `image_*` 清空 + `repairBoot=true`）→ 无条件部署引导层 → 单次启动 → 用户确认 → 重启 → 救援层跑 `bootfiles/zjswap-undo.sh`。
  · **`zjswap-undo.sh`（新建，随 initramfs 分发、0755）**：`set -u` + `say()` 走 stderr；守卫 = `~old` 与 `~old\Windows` 必须在（缺 → exit 2 绝不碰盘）、根下 `~new` 残留（exit 3）；阶段标记 `~new/.zj-undo-phase` 支持中断续做；L5 根→`~new`、L6 `~old/*`→根，**两步都跳过 `grldr`/`grldr.mbr`/`menu.lst`**（保 BIOS 引导链可回救援层续做）与瞬时项（直接 rm），`.zj-keepold` 搬回即删；最后 `rm -rf ~new ~old` + `sync` + `umount`。
  · **接入点**：`zjrestore-lite.sh` 顶层 `claim_bb_home` **之后**、引导日志清除与 `scan_image` **之前**读 `ACTION` —— 成功即 `RESULT=OK STEP=done` + `persist_log` + `exit 0`（不跑 bootfix：反向交换不改引导文件）。**安全网**：`action=restore`/空时挂目标查 `~new/.zj-undo-phase` → 中断后自动续做（防"标了契约却没跑完"变成死局）。失败按 rc 分支写 `STEP=returnold-failed`。
  · **交互（用户规格：重启前二选，取消是安全项）**：GUI 三选/两选框按钮词由「撤销恢复」改 **「返回旧系统」**；二选确认框 `CConfirmDlg::Ask2` **左=取消（默认，回车/ESC 都落这里）/ 右=立即重启**，且**先确认、后暂存** —— 否则用户取消后 BCD 里的 `bootsequence` 会在他下次手动重启时把机器带回救援层（不是他想要的）。CLI 新增 `SysRecover.exe returnold [--disk N --part M] [--yes]`（交互 `[y/N]`，取消=退出码 6；**非控制台必须 `--yes`**），help/总览同步。
  · **`--yes` 语义收窄（重要）**：残留扫描的 AskUndo 分支**不再 `--yes` 自动撤销** —— 撤销 = 重启机器，静默触发会让批量脚本把机房机器全部重启；改为打提示改用 `returnold`。keepOld 分支本来就不动（保留是用户显式选择）。非交互式一律保留不动并警告。
  · **移除 `UndoSwapLeftover`**（声明+定义，零调用方）：它就是问题 3 的根因，留着会被重新引用；`swapleft.h/.cpp` 原地留 PIT-139 注释指向取代者。
  · **问题 1 顺带（只读、不改行为）**：`selfdiag` 的 `diag` 新增两行 —— `[diag] third-party-tools:`（扫所有固定盘根，列 `LetRecovery_Scripts`/`OneKeyGhost`/`HotInstall`… 第三方装机工具残留）与 `[diag] ours-boot-assets:`（`grldr`/`grldr.mbr`/`menu.lst`/`ZJRESTORE`，与之对照）。定性：**我们从不创建也从不删除它们**，还原 = 如实写回镜像内容；有这两行即可当场回答"是不是你建的"。
  · **写演练时又抓出反向交换的两处续做真缺陷（都已修）**：① **`phase` 标记只写不读** —— 续做时 L5 会把 L6 已经搬回根的旧文件**再搬进 `~new`**，L7 一删就是静默丢数据；改为 phase=1（L5 前）/ phase=2（L5 后），phase≥2 续做**跳过 L5**。② **L3 一见 `~new` 就 exit 3** —— 自家暂存目录也进不来，续做永远失败；改为带 `.zj-undo-phase` 的放行。另加第三条：**L5 不搬契约文件**（`_zjresy*.log`/`restore-task.*` 留根）—— 若随 `~new` 一起被删，中断后根上什么都没有 → init 找不到任务日志（init 只在找到 log 或 `/tmp/restore-task.conf` 时才跑救援脚本）→ **安全网也无从触发**，用户再也进不去；成功后由 L7 清掉契约。再加 **L8 成功后置校验**（真看盘：根有 `/Windows` 且 `~new`/`~old` 均消失，否则非零退出）——"返回旧系统"成功与否不再靠脚本自说自话。
  · **QEMU 演练 4/4 PASS（2026-10-13，`0.7.31`）**：T2 完整反向（`zjsetup=2` 造"新系统在根 + `~old`（含 `.zj-keepold`）"后态 → 契约 `action=returnold` → `SWAPUNDO: phase=1` → `phase=2` → `post-check ok` → `RETURN-OLD DONE`，且全程**无 mkntfs/无 found image/无 apply**）；T3 中断续做（`zjsetup=3` 造 phase=2 中断态 → 日志 `resume: ~/.new/.zj-undo-phase phase=2` 且**无** `phase=2 (root moved…)` = L5 确被跳过 → 旧系统回根）；T6 回归 `run-swap-drill.ps1`（正向 swap + ESP/EA 子镜像）与 `run-drill.ps1`（B 引擎 block）均 PASS。新演练脚本 `tools/vmtest/run-undo-drill.ps1`（`-Resume` 切 T3）+ `mk-drill.py` 的 `ZJ_DRILL_UNDO` / `ZJ_DRILL_UNDORESUME` + `setup-swap.sh` 的 mode=1|2|3（init 的 `zjsetup=` 改为接受任意值并传参）。**为什么 mode=2/3 要直接造"交换后态"**：宿主是 Windows，挂不上救援层刚写的 NTFS，两跳之间改不了契约 —— 造后态就是绕开它。
  · **回归**：`make check` **48 用例 / 339 断言** + check-docs PASS + check-i18n **450/450**；`make package` 双架构（`0.7.31`）。新增单测 `task_returnold_action_in_all_builders`（三种文本形态都写 `action=returnold`、`engine=swap`、`image_*=0`、`contract_version=1` 不动）与 `task_action_defaults_to_restore`（空 action 回落 restore，老调用点不用改）。🚧 **T4 反向后真机启动**（QEMU 只验到"旧系统树回到根"，真机 BIOS/UEFI 能否启动旧系统待用户机房验证）。

### PIT-140

- PIT-140 **启动期弹框一律 `owner=nullptr` → 确认框藏在主界面背后、且不模态（用户 2026-10-13 虚拟机实测"这个界面必须显示在主界面之前，不选择不允许做其它事情"）**（`0.7.32`）：
  · **根因**：`src/gui/main_win.cpp` 里 `WinMain` 在 `ShowWindow` **之后**要跑两段启动期询问（光盘/U 盘搬迁 relocate、A 引擎残留分流 AskUndo），四处 `CConfirmDlg::Ask2/Ask3` 与配套 `MessageBoxW` **全部传 `nullptr` 作 owner** —— 于是对话框与主窗口是两个互不相关的顶层窗口：既**不在主窗口之上**（Win32 只保证"被拥有窗口在 owner 之上"，无 owner 就没有这条 Z 序保证），`RunModal` 里的 `EnableWindow(owner,FALSE)` 也**空转**（owner=nullptr 直接跳过）→ 主界面照样能点、能切走焦点，确认框被压在背后。`confirm_dlg.cpp::RunModal` 的注释一直写着"owner = 主窗口（模态禁用）"——**设计意图早就有，只是调用方没传**。
  · **修法**：① `StageReturnOldFlow` 加 `HWND owner` 形参，其内部 3 处 `MessageBoxW` + 1 处 `Ask2` 一并带 owner；② 启动期 4 个弹框（relocate 的 Ask2、复制失败 MessageBox、Ask3「处理备返的旧系统」、Ask2「检测到还原残留」）与 2 处 `StageReturnOldFlow` 调用全部传 `h`（= `pWnd->Create` 的返回值，`ShowWindow` 之后已在作用域内）；③ `RunModal` 里 `ShowWindow` 后补一行 `SetWindowPos(h, HWND_TOP, SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW)` —— 兜 `SetForegroundWindow` 在前台锁下失效的场景（只动 Z 序/激活，不移动不改尺寸）。
  · **为什么"在 ShowWindow 之后"这段必须带 owner**：PIT-104/问题2 那次是把这些块从"建窗之前"挪到"建窗之后"（否则界面出不来），但**挪位置时漏了把 owner 从 nullptr 换成新窗口句柄** —— 块挪了，对话框的从属关系没跟上。**教训**：窗口创建后新增的任何模态框，owner 必须是真实 HWND，`nullptr` 只在"窗口还不存在"时才合法。
  · **验证**：全仓复检无残留 `CConfirmDlg::Ask2/Ask3(nullptr`；`make check` **48 用例 / 339 断言** + check-i18n **450/450** + check-docs PASS；`make package` 双架构（`0.7.32`）。🚧 待用户虚拟机复测：确认框应在主界面之上，且未选择前主界面点不动。
  · **另两个同批问题（未定性，待日志）**：① 万能镜像装完"桌面没图标"（怀疑是镜像自身内容，待核 `C:\Users\<用户>\Desktop` 与 `Public\Desktop`）；② 「确认返回后进 Linux 停在 `#`」—— 屏显 `RESULT: restore FAILED (rc=1)` + `blackbox sweep start (reason=interrupted)` + `WARN: no writable surface found for logs` = 救援脚本已执行但返回 1，且**黑匣子没落盘**（`/tmp/zj-bb.done` 为空），故日志只在 RAM/兜底窝里；取证命令见下条回复给用户的话术。

### PIT-141

- PIT-141 **「返回旧系统」失败时证据不足 → `zjswap-undo.sh` 的 `say` 补写 `$LOG` + `runerr()`；`zjrestore-lite.sh` 的 returnold 分支先停 mirror 再执行 undo**（2026-10-13，`0.7.33`；配合 PIT-142 的诊断）。PIT-140 记录的"② 确认返回后进 Linux 停在 `#`"：屏显 `RESULT: restore FAILED (rc=1)` 但**黑匣子没落盘**（`/tmp/zj-bb.done` 为空）→ 事后无文件证据。两条修复：① `zjswap-undo.sh` 的 `say()` 追加写 `$LOG`（`/tmp/zjrestore.log`，init 会在失败时重打到屏幕 + 落盘）；新增 `runerr()`（替代裸 `cmd 2>/dev/null`，失败时取回原因头部 300 字节）；L5/L6/L7 的 `mv`/`rm`/`umount` 全部改走 `runerr`。② `zjrestore-lite.sh` 的 returnold 分支在调用 `sh "$ZJSWAPU"` **之前** `stop_log_mirror` —— mirror 每 2s 往 `~new/ZJRESTORE-logs/` 写，undo 的 L5 会把该目录搬进 `~new`、L7 再 `rm -rf ~new` → mirror 边删边长（PIT-142 的直接诱因）；先停 mirror 让 L7 能删干净。**教训**：救援层"自删除目录"场景下，必须先停掉任何还往该目录写文件的后台进程。

### PIT-142

- PIT-142 **「返回旧系统」必失败（客户实测 rc=1）根因：L5 把目标根的诊断资产 `ZJRESTORE-logs/` 搬进 `~new` → L7 `rm -rf ~new` 边删边长（log mirror 每 2s 写）→ 永远删不干净 → L8 后置校验 exit 1**（2026-10-13，`0.7.33`→`0.7.34`；QEMU 演练复现 + 修复 + 对照实验全过）。PIT-141 只补了日志，没解决"为什么 rc=1"。逐层排查：
  · **演练复现**：`run-undo-drill.ps1` 加 `plant_bb_home()`（`setup-swap.sh` 往目标根写 `ZJRESTORE-logs/zjrestore-debug.log` + `ZJRESTORE-status.txt`，模拟真机"软件目录找不到 → 日志窝落目标根"）→ 演练立刻复现 `WARN: diagnostic asset moved into ~/.new` → L7 `rm -rf ~new` 超时 → L8 `~new-still-present` → exit 1 → `RESULT: restore FAILED`，与客户现场一字不差。
  · **为什么 L5 会搬它**：`zjswap-undo.sh` 的 L5 循环遍历根下所有条目，跳过清单只有 3 条 —— `is_bootload`（grldr/grldr.mbr/menu.lst）、`is_contract`（`_zjresy*.log`/`restore-task.conf|json`）、`is_transient`（仅 5 项：`$RECYCLE.BIN`/`System Volume Information`/pagefile/swapfile/hiberfil）→ `ZJRESTORE-logs` **不在任何一条** → 被当"待丢弃的新系统"搬进 `~new`。
  · **修法（用户 2026-10-13 规格：日志优先原软件目录，找不到时用 `~new` 之外的兜底/临时目录，绝不写进会被删的 `~new`）**：新增 `is_diag()`（跳过 `ZJRESTORE-logs`/`ZJRESTORE-*.log`/`ZJRESTORE-*.txt`/`ZJRESTORE-*.out`/`zjrestore-boot.log`）并**在 L5 的 `is_transient` 之前**接入跳过（放前面防误入瞬时分支被 `rm` 掉）→ 诊断资产留在目标根（= 用户说的"兜底目录"），L7 删 `~new` 时无阻碍。**检测线保留**：万一 `is_diag` 漏了名字，L5 后仍打 `WARN: diagnostic asset moved into ~/.new: …`（对照实验：加 skip 前 WARN 命中 2 条，加后 0 条，演练 PASS）。
  · **验证**：`bash -n` 过；`build-debian-rescue.py` 重建（494 模块）；QEMU 演练 **15/15 PASS**（含 `post-check ok: old system at root, ~/.new and ~/.old gone`、`RETURN-OLD DONE`、`restore script returned (rc=0)`，且日志持久化到 sda1 兜底目录、不在 `~new`）；`make check` 48 用例/339 断言 + check-i18n 450/450 + check-docs PASS。✅ 2026-10-13（`0.7.34`）。
  · **教训**：① "自删除/自清理目录"（`~new`）的跳过清单，要按"**哪些东西不该被搬走**"逐条穷举，不能只列已知的 bootload/contract —— 演练盘上多一个 `ZJRESTORE-logs` 就破防；② 真机日志窝可能落在目标根（PIT-111/137 的兜底路径），returnold 脚本必须把它当"根的一部分"而不是"新系统残留"；③ 排查手法：QEMU 演练盘上 `plant_bb_home` 模拟真机兜底路径 → 一条命令复现 → 修完对照 WARN 消失 + 全量断言 PASS。

### PIT-143

- PIT-143 **演练时有时无假失败 = 契约日志缺失时无条件全量扫 4000 模块×2 遍（60-120s）卡在捕获窗边缘；+ fd0 软驱进入全部分区扫描循环。修 = conf 已找到即跳过 sweep + 消费端全线排 fd/floppy**（2026-10-14，`0.7.35`；P0-A 稳定基线期间）：
  · **现象与定因**：undo 演练 `-RestoreSecs 120` 下一轮 15/15 PASS、一轮 8 项 FAIL。三轮日志 diff：PASS/FAIL **前缀完全相同**（`no _zjresy*.log found` → `found conf: /tmp/zj_m/restore-task.conf` → `no task log -> sweeping ALL modules`），分叉点只在 FAIL 轮**死于 sweep 中途**（无 `sweep pass done`、无脚本输出、日志尾停在 kernel 时间戳 ~76s 即 120s wall 被杀）→ 边缘超时，非逻辑错。而 conf 已找到 = 分区可枚举 = 存储驱动没问题 —— sweep（PIT-083 H-02 兜底："分区不可见才补驱动"）在这条路上纯属白扫 2 分钟。
  · **修复 A（`alpine/init`）**：`no task log` 分支先查 `/tmp/restore-task.conf` —— **有 conf → 跳过 sweep**（打 `no task log but conf found -> skip module sweep (partitions enumerable)`）；**log 与 conf 两条线索全丢**才全量扫（保住 H-02 本意）。修后连续 2 轮 PASS。
  · **修复 B（fd/floppy 消费端全线排除）**：QEMU `/dev/fd0` 进入所有 `list_parts` 消费循环 —— 其中 `target_by_offset` 的 MBR 兜底曾**裸 dd**（PIT-142 现场实测阻塞 ~400s，已先加 `timeout 10`）；本次统一补 `*fd*|*floppy*` 跳过：`zjrestore-lite.sh` 的 `bb_sweep`/`claim_bb_home`×2/`persist_log` 兜底/`cleanup_after_success`/`find_soft_dir`/`target_by_offset`/`scan_image`（**保留 `sr*`**：镜像可能在光盘，注释在案）；`init` 的 `scan_for_log`/conf 搜索/bootfix 扫描。**`list_parts` 本身不动**（A9 设计：fd0/sr0 作可移动整盘故意列出）；`find_esp_dev` 已有 `is_removable` 天然排除。
  · ⚠️ **演练工作流坑（本次踩到）**：`run-drill.ps1` **不建盘**，直接用 `base/drill.raw` —— 若刚跑过 undo 演练，盘上契约是 `action=returnold` → block 演练必 9 项 FAIL（`SWAPUNDO: ~old absent`）。**跑 block 演练前必须 `ZJ_DRILL_ENGINE=block` + `mk-drill.py` 重建盘**（run-undo/run-swap 内部自建、run-drill 不建）。
  · 🚧 **未闭环**：3 轮（含 PASS）init **都**没扫到演练盘 sda2 上的 `_zjresy-drill.log`（恒 `no _zjresy*.log found` → 恒走 conf 回退）——疑似 `setup-swap.sh` mode=2 清根时删掉了它，待查（真机 returnold 是 StageReturnOld 后写 log，不受影响）。
   · 回归：undo ×2 PASS、block（重建盘）PASS、`bash -n` 双脚本过、initramfs 重建（494 模块）、`make check` 48 用例/339 断言 + i18n 450/450 + check-docs PASS、`make package` 双架构。✅ 2026-10-14（`0.7.35`）

### PIT-144

- PIT-144 **bulk 5 万文件压测：undo L7 `rm -rf ~new` 必失败 = ntfs3 驱动 rename 风暴后留"幽灵索引项"（readdir 空但 rmdir 恒 ENOTEMPTY，重挂载也清不掉）；修 = `zjswap-undo.sh` 改 ntfs-3g(FUSE) 优先 + `rm_rf_retry`**（2026-10-14，`0.7.36`；P0-A 压测期间）：
  · **现象（三轮定因）**：`run-undo-drill.ps1 -Bulk 50000 -RestoreSecs 900`、ntfs3 挂载下 L5（5 万次 rename，bulk 平铺在目标根）→ L7 删 `~new`：① 首轮 4 个空文件（`f3912/f42318/f669/f8144`）unlink 持续失败；② 加 sync 重试 ×4 无效（同 4 个文件）；③ 第 3 轮加"第 2 轮重挂载"——条目**消失**（`ls` 空、取证行 `leftover:` 空）但 rmdir 仍报 `Directory not empty` → **盘上 INDX 幽灵项**（readdir 返回不了、rmdir 照样数它），非懒刷写竞态、非并发写入者（log mirror 在 sda1 兜底窝，不碰 ~new）。
  · **修复（双保险）**：① **挂载顺序翻转** `mnt_undo()`：`ntfs-3g -o force` 优先、`mount -t ntfs3` 兜底 —— 本脚本全程只做 rename + 递归删除，正确性优先；顶层 rename 不随子树规模放大，50k 实测代价 +160s（401→548s）。② `rm_rf_retry()`：3 轮 `sync+2s` 重试 → 第 2 轮先 `umount`+重挂载（取证行含 umount 失败原文）→ 全败后逐条抓 unlink errno + 抓 `rmdir` 原文 + `ls -a count`（为下次定因留证，不再靠盲猜）。
  · **结果**：修后 bulk 50000 **零重试一轮过**（`post-check ok` → `RETURN-OLD DONE`，548s）；常规 undo 演练默认 120s 窗口 PASS（FUSE 对小树无感）。
  · ⚠️ **只改了 undo 脚本**：生产 restore 路径 `zjrestore-lite.sh::mnt_dev` 与正向 `zjswap.sh` 仍 ntfs3 优先 —— 常规（非压测）场景 ntfs3 一直通过，换驱动顺序需单独实证，无证据不扩大改动面（PIT-102 同款纪律）。
  · 回归：bulk 50000 PASS + 常规 undo PASS、`bash -n` 过、initramfs 重建（494 模块/15.9MB gz）、`make check` 48 用例/339 断言 + i18n 450/450 + check-docs PASS。✅ 2026-10-14（`0.7.36`）

### PIT-145

- PIT-145 **统一诊断底座 `zjlog.sh`（P0-B）：5 套 say() 收敛单源 + 3 处裸 mount 收敛 `mnt_ntfs` + 构建期成套断言**（2026-10-14，`0.7.37`；用户口径："从源头把各种可能考虑到，别老让用户卡 `#` 回来要日志补一个"）：
  · **背景（盘点）**：`init`/`zjrestore-lite.sh`/`zjswap.sh`/`zjswap-undo.sh`/`setup-swap.sh` 各自复制了一份 `say()`，行为渐渐漂移 —— init/lite 文件行**无前缀**（多脚本合并的 `/tmp/zjrestore.log` 分不清谁说的）、undo 文件行带前缀（PIT-141 补的）、swap/setup **只打控制台不落文件**（/tmp 里直接缺行）。另有 3 处裸 mount 无 timeout（`zjswap.sh`/`setup-swap.sh`/`zjswap-undo.sh::mnt_undo`）—— PIT-112"ntfs3 挂坏设备静默挂死 400s"的纪律只覆盖了 `zjrestore-lite.sh::mnt_dev`。
  · **修复（`bootfiles/zjlog.sh`，新文件）**：① `say()` 单源 —— 控制台 `<前缀>: msg`（**前缀是演练断言/录像取证契约，必须保真**：`SR:`/`ZJ:`/`SWAP:`/`SWAPUNDO:`/`SETUP:`），文件行 = `时间戳 前缀: msg` 追加 `${LOG:-/tmp/zjrestore.log}`（5 脚本合并一条可分辨时间线，LOG 未导出的子进程兜底同路径），写失败静默且固定 `return 0`（`set -u` 安全、不让 `if say` 判断走偏）；② `mnt_ntfs <dev> <mnt> [ntfs3|fuse]` —— 三个裸 mount 收敛点：每步 `timeout 60` + stderr 落 `/tmp/zjmnt.err`（不再被 `2>/dev/null` 静默吞）、成功打 `mnt <dev> ok (<drv>)`（**这次是 FUSE 还是内核驱动是 PIT-144 类问题的定罪要素**）、`fuse` 序 = ntfs-3g 优先（zjswap-undo.sh 专用，PIT-144）；③ **有意只放 say**：现有调用点是"say ERROR → umount → exit N"多步形态，`die`/`run` 硬套只增 diff 噪音，需要时再加。
  · **5 处接入**：各脚本删本地 `say()` → `ZJPREFIX=<前缀>` + `. /zjlog.sh`（`probe()`/`say_dmesg()`/`runerr()`/`dump_console()` 等周边函数留在各自脚本）；`build-debian-rescue.py` 打包列表加 `('zjlog.sh', 0o755)` + **构建期断言**（镜像里有 `zjlog.sh` 且 5 个消费脚本都含 `. /zjlog.sh`，缺一 `raise` —— 漏打的后果是所有脚本启动即 `say: not found`、日志全灭，正是本轮要消灭的静默降级，同 PIT-079 fail-fast 纪律）。
  · **演练工作流坑（本次又踩，同 PIT-143 教训）**：`run-swap-drill.ps1` **不建盘**（头注释有配方：`ZJ_DRILL_ENGINE=swap` + `mk-drill.py`），复用上一轮 undo 演练留下的盘（mode=2 且 `zjsetup` 已被 setup-swap.sh 成功后 `sed` 剥掉）→ init 见不到 `zjsetup=` 直落 `#` → `setup boot FAIL`。**跑前必先按各自配方重建盘**；`run-drill.ps1` 同理（PIT-143 已记）。
  · **回归（三演练全绿）**：undo **16/16**（含 `phase=1/2`、`post-check ok`）、swap **12/12**（`SWAP: mnt /dev/sda2 ok (ntfs3)` 新回执在案、`engine=swap`/`PIT-138 image re-scan`/`RESTORE DONE`）、block **9/9**（B 引擎全链 + EA/ESP 黑匣子）；`bash -n` ×6 全过；`make check` 48 用例/339 断言 + i18n 450/450 + check-docs PASS（`0.7.37`）。日志噪音 `no writable surface` 经历史 PASS 轮比对为既有环境波动（hits=4/0 交替出现），非本次回归。✅ 2026-10-14（`0.7.37`）

### PIT-146

- PIT-146 **救援层能力自检 `caps_selfcheck`（P1）：动 target 之前先验证"环境能不能干这件事"，缺件以 CAPS 行 fail-closed**（2026-10-14，`0.7.38`；用户口径"从源头把各种可能考虑到，不能老让用户卡 `#` 回来要日志补一个"的 P1 落地）：
  · **动机**：打包缺件（wimlib/mkntfs/ntfs-3g/blkid 被裁）、引导脚本缺失这类**源头缺陷**，原实现要等 mkntfs/apply/ESP 段以怪异 rc 失败才发现（甚至静默）——现在统一在**动 target 之前**用结构化 `CAPS` 行报出。
  · **位置**（`bootfiles/zjrestore-lite.sh`）：`claim_bb_home`（日志出口已认领）与 returnold 检测**之后**、三条执行路（`zjswap-undo` 反向交换 / `zjswap` 正向交换 / B 引擎 mkntfs+apply）**之前** —— 一处覆盖全部。
  · **致命项（缺失 → `RESULT=FAILED STEP=caps-missing` 退出，此时还没碰目标分区）**：① 目标是块设备 + **可写打开探测**（`dd of=$TARGET_DEV count=0` = 纯 `O_WRONLY` 打开、零字节写入、无副作用，提前暴露只读盘/写保护）；② `blkid`、`ntfs-3g`（所有挂载路径的公约数：mnt_dev 兜底/B 引擎 libntfs-3g 写卷/undo 的 FUSE 专用）；③ 引擎分路 —— undo 要 `zjswap-undo.sh`、swap 要 `zjswap.sh`（契约带 esp/ea 子镜像时还要 wimlib，PIT-138）、block 要 wimlib+mkntfs；④ 块设备可见（`list_parts` 非空，防存储模块/打包回归）。
  · **非致命（仅 `CAPS warn:` 照常继续）**：`target_part_offset` 为空（target 来自 init 已校验的 `$1` 时属正常）、`MemTotal<400MB`（PIT-113 OOM 边界）、`claim_bb_home` 没 claim 到日志出口（末尾 `bb_final` 仍全盘扫，但中途失败证据可能只在屏幕/内存）。
  · **工具探活语义（防误杀）**：`_capbin`/`_wim_ok` = `command -v` 存在 + `--version` 实际执行，**只有 rc=126/127（无法执行/找不到）判缺失** —— `--version` 返回其它非零码不误判（各工具版本旗标行为不一）。wimlib 解析与 L758 同口径（PATH → `/tmp/zjtools` 老底座兜底）。
  · **报告样例**：`ZJ: CAPS engine=block undo=0 target=/dev/sda2` + `CAPS result=ok`；失败时 `CAPS result=MISSING: wimlib-imagex mkntfs` + `STEP=caps-missing`（`trap bb_final EXIT` 照常落黑匣子，`persist_log` 已调）。对用户 = 开机第一屏就看到"缺什么"，不再靠事后要日志。
  · **回归**：undo/swap/block 三演练 PASS，三处日志各见 `CAPS engine=…` + `CAPS result=ok`（undo 演练 `undo=1`、正向 `undo=0`）；**负例** `bash tools/vmtest/test-caps-failpath.sh`（git-bash 抽函数块 + 桩环境跑 MISSING 分支：exit 1 / MISSING 列表 / persist_log / 不越过检查，13 断言 PASS；⚠️ 断言别把 `check` 放进管道——管道元素在子 shell、fail 标志会丢，本脚本首版即栽过）；`bash -n` 过；`make check` 48 用例/339 断言 + i18n 450/450；initramfs 重建 stamp 0.7.38。🚧 未在 QEMU 层剔除单个工具复现（构建脚本无该钩子；函数级负例已覆盖失败分支）。
  · 附带：Makefile `package` 补 `zjlog.sh` → `dist\bootfiles\`（PIT-145 漏拷，运行期靠 initramfs 内副本不受影响，但交付树不齐）。

### PIT-147

- PIT-147 **P2：`2>/dev/null` 全量分类清理（245 处盘点，B 类 12 处修掉）+ shell 门禁三件套挂 `make check`**（2026-10-14，`0.7.39`；P0-B/P1 之后的"源头健壮性"收尾）：用户口径"从源头把各种可能考虑到"——把救援层 6 个脚本里每处 `2>/dev/null` 过了一遍（lite 159 / init 57 / undo 18 / zjswap 12 / setup-swap 3 / zjlog 1，纯注释行不计），按「A=探针/尽力而为/有读回校验或响亮兜底，保留；B=消音会藏真故障，修掉」定案：
  · **B 类 12 处（全部修掉）**：① lite `START_LBA` 非法回落 2048 → 响亮 WARN（原先静默换值，BPB 隐藏扇区就错）；②③ lite/init 的 `nvme io_timeout` 写失败 → WARN（PIT-055 性能旋钮静默失效）；④ lite **成功回执**双直写（软件目录/兜底窝）全失败 → WARN（否则连 ESP 也没落上时，下次 Windows 启动把 PIT-115 pending 误报成"任务未执行"）；⑤ zjswap `rm -rf ~new` 失败 → WARN；⑥⑦ undo `phase=1/2` 标记写失败 → ERROR（resume 靠它跳过 L5，静默失败=续做可能重搬已还原旧文件再删=丢数据）；⑧ init conf `cp` 失败仍说 "found conf"（骗人日志，下游白找一轮）→ 只有 cp 真成功才说、失败 WARN；⑨ init bootfix `cp` 同款；⑩ init 引导日志写目标根失败 → WARN（原先 `&& say` 无 else，关键诊断载体静默丢失）；⑪ bb_mount 挂载链失败原因原样丢 `/dev/null` → 收进 `/tmp/zjbb.err` 且全败时 `say` 回吐头两行（"no writable surface" 从此带原因）；⑫ 同批把 bb_mount 5 处 mount/ntfs-3g 消音改为路由错误文件（lite 计数 165→159）。
  · **A 类（保留，分类哲学=以后新增消音的判据）**：探针读（`find/grep/cat/dd if=` 等失败=空值，调用方都有守卫）；`command -v xxx 2>/dev/null`（探测不是执行）；sweep 全量 `modprobe`（4000 个模块逐个试，失败=正常噪音，结果看随后的 block devs 列表）；PBR/hidden-sectors 的 dd 写（读回校验断言是真检查，dd stderr 是重复信息）；`zjlog.sh` 的 `say()` 自身消音（有意设计：say 不得破坏脚本）；umount/rm 幂等清理兜底链等。
  · **门禁 `tools/check-shell.py`（已挂 `make check`，零依赖）**：C1 `bash -n` 7 个脚本（6 产品 + 负例 harness）；C2 **消音计数棘轮** —— 每文件 `2>/dev/null` 次数 ≤ `tools/shell-stderr-baseline.txt`（只许降不许升；新增消音必须先在 PIT 里完成 A/B 分类审查再 `--update-baseline`，diff 可见）；C3 **禁消音名单 8 命令**（`wimlib-imagex mkntfs ntfsfix ntfsresize wipefs sfdisk fdisk ntfs-3g`，失败必须原样吼出；按段首判定，`timeout N`/`!` 前缀计入、`command -v` 探测豁免——`grep mkntfs 2>/dev/null` 这类参数误命中也不算）。
  · ⚠️ **本批自踩（当场修）**：基线文件格式是「次数 路径」，解析器却按「路径 次数」找 `isdigit()` → 恒报 "baseline missing/empty"（写和读不同口径——三端同口径纪律第 N 次应验）。
  · **回归**：`bash -n` 7 过；三演练重建盘后全绿（undo **16/16**、swap **12/12**（`CAPS engine=swap undo=0`+`result=ok` 在案）、block **9/9**（全量日志确认 CAPS 行））；failpath 负例 13 断言 PASS；`make check` 48 用例/339 断言 + check-docs/i18n 450/450 + **check-shell PASS（159/12/18/3/1/57 全 ≤ 基线）**；initramfs 重建 stamp 0.7.39。✅ 2026-10-14（`0.7.39`）

### PIT-148

- PIT-148 **P3 失败自动回退全链（STAGE 决策 + AUTO-RECOVER 演练 + Windows「失败原因+解决办法+一键重试」页）**（2026-10-14，`0.7.40`→`0.7.41`；用户批准方案五项，"按计划继续推进不要中途问"）：用户口径"任何失败都要能回到可用状态，不能让用户卡在 `#` 裸 shell 回来要日志补一个"。
  · **救援层（0.7.40，本会话前半已完成）**：`zjrestore-lite.sh` 引入 **STAGE 模型**（0=未动目标 / 1=文件就位但引导未验证 / 2=bootfix 已验证 / 3=已格式化未 apply / 4=returnold 进行中），`bb_final` 按 STAGE 决策退出行为 —— `0/2 → reboot -f`（A/B 类自动重启回 Windows）、`1 → 引导菜单`（三选 `[1] Retry [2] Reboot [3] Expert shell`）、`3|4 → 自动重试`（`/tmp/zj-retry` 计数 ≤2，用尽落菜单）；`init` 尾部重写 `emit_result`/`reboot_now`/`menu_loop`，重试用尽不再裸 `#`。A/B 类 STEP 全集 = `no-task / contract-mismatch / target-not-found / caps-missing / image-not-found / unsupported-image / interrupted / swap-missing`；C 类（已动盘）= `mkntfs-failed / apply-failed / swap-failed / returnold-*`。`mk-drill.py` 新增 `ZJ_DRILL_AUTORECOVER=1`（conf image_path 指向不存在文件）；新演练 `run-autorecover-drill.ps1` **8/8 PASS**（失败 → 自动重试 → 引导菜单 → 退出菜单可续），回归 block 9/9 / swap 12/12 / undo 16/16 全绿。
  · **Windows 侧（0.7.41，本会话落地）**：① `common/rescue_decision.h` 新增纯逻辑 **`RescueStepRetryable(step)`**（A/B 可重试、C 类 fail-closed、未知 step 不提供）+ 单测 `rescue_step_retryable`（16 断言，守"别把 mkntfs/apply 混进可重试"边界）；② `selfdiag` 的 `CheckLastRescueFailure` 改返回 **`RescueNotice{msg, retryable, step}`**（A/B 失败与"任务未执行"标记 → retryable），`StatusStepText` 补 6 个新 STEP 中文化；③ 契约**增量键 `keep_old=%d`** 进 conf/log（`RestoreTask::keepOld`，老救援层不读、不升 contract_version），单测 `task_conf_keep_old`；④ 新增 **`ops::RetryStagedRestore`**（读 `<exeDir>\restore-task.conf` → 拦 `action!=restore` → 按 offset/size/serial 匹配盘（盘符不可靠，PIT-128 同款纪律）→ 重建请求（engine 缺键=block、keepOld 恒 false）→ 复用 `StageRestore` 全流程 → `AppendHistory("restore-retry")`；⑤ GUI：回执弹 **`Ask3` 三选「知道了 / 打开日志 / 一键重试」**（C 类保持 Ask2），一键重试先二次确认（默认取消=安全项）→ worker（BELOW_NORMAL + `m_retryErr` 成员回传 + **`WM_RETRY_DONE`=WM_APP+18** 避让 Duilib +1）→ 成功且需重启自动 `RebootNow()`（PIT-110 同款）、失败 MessageBox fail-safe；⑥ CLI 适配 `RescueNotice` 并附"可用 GUI 一键重试"提示。**i18n 新增 25 键 → 475/475 全译**；`--skeleton`/`--gen-lang` 全流程走通。
  · **回归**：`make check` **50 用例 / 358 断言**全绿 + check-i18n 475 + check-shell PASS（基线不动）+ check-docs PASS；待 `make package` 双架构 + AGENTS 版本对齐。🚧 待实测：真机造一次 `image-not-found` 失败 → 下次启动三选框点「一键重试」→ 自动重暂存并重启。
  · **边界（有意）**：重试 `keepOld=false`（要备返请走正常流程）；C 类只给日志路径（救援层已自动重试 ×2，在半成品盘上再"一键重试"不可控）；`RetryStagedRestore` 不写 pending 标记以外的新契约 —— 走 `StageRestore` 既有契约写入（A2 清场一并生效）。

### PIT-149

- PIT-149 **「删除旧系统」必闪退（`~old` 一个字节没删、下次开机又被问一次）根因 = `LocalFree` 释放了 `GetNamedSecurityInfoW` 返回的 DACL **内部**指针 → `STATUS_HEAP_CORRUPTION` 秒杀进程**（2026-10-17 双分区 e2e 虚拟机定位 + 宿主最小复现，**产品源码修复** `src/app/swapleft.cpp`，`0.7.57`）：
  · **现象**（e2e S5：A/B 交换后首启、残留三选框点「删除旧系统」）：点击确实命中（`clicked DELETE (right btn) at client 340,161`），随后**主窗与进度窗一起瞬间消失**、`~old` 原封未动、`C:` 占用不变；日志停在 `swap leftover: C: ~old -> delete (user chose, keepold)` → `tree delete begin: C:\~old` → `takeown begin #1: C:\~old`，**无 `takeown done #1`**、无 `rec enter`、无 `message loop exited`。用户视角 = 「点了删除没反应/闪退，下次开机再问一次」。
  · **为什么难查**：**没有任何崩溃证据** —— 无 `Minidump`/`MEMORY.DMP`/`LiveKernelReports`（**不是蓝屏**）、无 WER `ReportQueue/ReportArchive`、自家 `logs\crash\` 也空（UEF/VEH 从未被调用）= **fastfail 式终止**（`0xC0000374` 这类"关键失败"由 ntdll 直接终结进程，绕过 `SetUnhandledExceptionFilter`），只能靠「窗口全消失 + 日志停在 takeown 内」反推（期间曾被"黑屏"误导过一次，实为**显示器休眠**：唤醒后控制台仍在、tick 持续到 t=936s，客机**没有重启**）。
  · **根因**（`TakeOwnershipAndGrant`）：取 DACL 用
    `GetNamedSecurityInfoW(ep, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &oldDacl, NULL, NULL)`，随后 `if (oldDacl) LocalFree(oldDacl);` —— **`ppSecurityDescriptor = NULL` 时函数仍会分配安全描述符（并被泄漏）**，返回的 `oldDacl` 是**该描述符内部**的指针；单独 `LocalFree` 内部指针 → **堆损坏 → 进程被 fastfail 终结**。
  · **最小复现（宿主、零依赖）**：`tools/vmtest/e2e/takeown-probe.cpp`（`buggy|fixed <dir>`，逐步骤 flush 日志）—— `buggy`：`get-dacl(sd=NULL) rc=0 oldDacl=…794` → `set-dacl rc=0` → `before LocalFree(oldDacl)` **之后无输出**，退出码 **`0xC0000374`**；`fixed`：`sd=…8C10 oldDacl=…8C24`（**证实 `oldDacl = sd + 0x14` 内部指针**）→ `LocalFree(sd)` → **exit 0**。
  · **修法**（2 行）：`GetNamedSecurityInfoW(..., &oldDacl, nullptr, &oldSd)` + `if (oldSd) LocalFree(oldSd);`（**不再**释放 `oldDacl`）。同批 grep 全仓确认该误用**只此一处**（删除旧系统路径），与历史「删除旧系统无效」的反馈完全吻合。
  · **回归**：S5 用 `0.7.57` 重跑全绿 —— `clicked DELETE` → `takeown done #1` → `swap leftover del progress: ok=500…1000…`（**ok=10400 fail=0**）→ 完成框「旧系统已删除，释放磁盘空间。」 → **`~old present: False`**、`C:` 占用 14GB→**10GB**、`zjgui\auto-b.log` 记 `SUCCESS: C:\~old is gone`（**S6 PASS**）；离线取证**无 Minidump/WER/自家 dump**（本次未崩溃）；`make check` 50 用例/358 断言 + check-docs/check-i18n(475)/check-shell/check-widths 全过；`make package` 双架构。
   · **教训**：① `GetNamedSecurityInfo(W/Ex)` 家族**只有 `ppSecurityDescriptor != NULL` 时**返回的地址才是可 `LocalFree` 的描述符；`DACL/SACL/OWNER/GROUP` 输出参数一律是**内部指针**，禁止单独释放（正解永远是 `LocalFree(sd)`）。② 「**无 dump 的秒退**」应往**堆损坏 / fastfail**（`0xC0000374`、`0xC0000409`）方向查，而不是怀疑崩溃处理器没装 —— fastfail 根本不给用户态处理器机会。③ 「无文件证据」≠「没发生」：窗口全消失 + 日志停在某函数内部，就是该函数的罪证；宿主最小复现是定根因最快的路。

### PIT-150

- PIT-150 **日志落地规则改写（四级链 + `<盘>:\ZJRESTORE` 隐藏）+ 克隆首启全自动「先搬日志、再问删不删旧系统」**（2026-10-17，**产品源码改动**，`0.7.58`→`0.7.61`；用户规格四段原话）：
  · **用户规格**（2026-10-17）：① 日志根 —— "有 `C:\Program Files\SysRecover\logs` 就放这里，没有就放软件所在文件夹，没有文件夹（光盘）情况下仍然放到除系统盘外的硬盘分区里的第一个可写分区的 `\ZJRESTORE`，如果一个都没有可以向用户弹出错说明。但 `D:\ZJRESTORE` 应设置隐藏"；② 备返（keepOld）克隆成功后**首启全自动**：自动运行本程序 → 把日志搬进程序目录 → **然后**才弹「是否删除旧系统」；③ 自动运行通道用 **RunOnce**（登录后弹一次 UAC，用户已接受）；④ 搬运失败只提示不阻断。
  · **`LogBaseDir()` 重写为四级链**（`src/common/relocate.cpp`）：① 已安装的程序目录 `<系统盘>\Program Files\SysRecover`（可写就用，**WOW64 下 `%ProgramFiles%` 仍是原生目录**，与部署路径一致）；② 程序所在文件夹（U盘/绿色目录）；③ 只读介质 → 除系统盘外**第一个可写固定分区**的 `\ZJRESTORE`（沿用两轮扫描：优先不含 Windows 的盘、D: 优先）+ **设 `FILE_ATTRIBUTE_HIDDEN`**；④ 一个都没有 → 退回程序目录 + **粘性标记 `LogHomeDegraded()`**（`LogWarn` 落 `.log`，并由既有的"程序在只读介质且无盘可写"对话框把"日志无法保存"讲给用户 —— 该情形必然触发 `CheckRelocate` 的确认框，故**未新增 UI 字符串**）。**PE 例外**：PE 的 `GetWindowsDirectoryW()` 在 RAM（`X:\Windows`），而规则 ①/② 里的 `C:\Program Files\SysRecover` 可能是**离线系统盘**（还原目标）→ 前者经 `UseInstallDirLogHome(..., isPe=true)` 直接否决、后者靠 `isPe && AppDir 在 windir 盘` 跳过 → PE 行为与 PIT-105/107 完全一致（日志仍落数据盘）。PE 判定在 `common` 层**复制**了 `disk.cpp::IsWinPE` 的 MiniNT 判据（守住依赖 DAG：common 不依赖 disk）。
  · **纯逻辑抽函数 + 单测**：`UseInstallDirLogHome(installDir, winDir, isPe)`（不同盘/空路径/非法路径/PE → false），新增 `TEST(log_home_install_dir_rule)` 8 断言 → `make check` **51 用例 / 366 断言**。
  · **首启搬迁 `MigrateLogsToLogHome()`**（`relocate.cpp`，**仅当规则 ① 命中**才动手）：源 = 各固定盘（不含系统盘）的 `<盘>:\ZJRESTORE\logs`（PIT-059 软件目录回退）与 `<盘>:\ZJRESTORE-logs`（PIT-137 黑匣子兜底窝，文件**直接摊在根下**）；目的 = 日志根 `\logs`。**绝不覆盖**同名文件（`UniqueDest` → `名字 (2).ext`）；跨卷/被占用退化为"复制 + `MOVEFILE_DELAY_UNTIL_REBOOT` 登记开机删除"；搬空的 `logs\`（进而空的 `ZJRESTORE\`）删掉，没搬空的（还有 `bootfix\`/`rescue-build.txt` 等）与兜底窝**只设隐藏**；一次调用落一行 `log migrate: moved=N failed=M sources=K dest=…` 便于现场取证。
  · **调用点**（`src/gui/main_form.cpp::InitWindow`，在 `ProgressInit(logsDir)` 之后、`StartupPhase` 之前，由 `CloneFirstBootPending()` 门控）：**必须早于懒加载**（`WM_STARTUP_LAZY` 线程里的 `CollectDeployArtifacts`+`CleanupStrayLogs` 会先把源目录收走/清掉，晚一步就搬不到东西且互相竞态 —— 最初放在 `main_win.cpp` 的 Ask3 三选框内，实测会被收集器抢先）。**有意只在 keepOld 首启分支调用**（`CloneFirstBootPending()` = 任一固定盘存在 `<盘>:\~old\.zj-keepold`），不做"每次启动按不变量搬迁" —— 否则在**老系统暂存运行**时会把 `<数据盘>\ZJRESTORE\logs` 搬进 `C:\Program Files\…`，紧接着 C: 就被格式化 → 白搬 + 丢证据。keepOld=false（静默删 `~old`）路径无 `~old`/无标记，结构上触发不到（**本轮有意不做**）。
  · **自动运行通道（`src/app/ops.cpp`）**：把原来 keepOld=false 的内联 RunOnce 注入**重构成共用助手 `InjectOfflineRunOnce(newDir, hiveName, valueName, cmd, type, what)`**（挂离线 `~new` SOFTWARE hive，需 `SeBackup/SeRestore` 特权，失败只 WARN），keepOld=true 分支在桌面快捷方式之后注入 `ZJSysRecoverFirstBoot` = `"%SystemDrive%\Program Files\SysRecover\SysRecoverUI.exe"`（**REG_EXPAND_SZ**，让**新系统**自己展开 `%SystemDrive%`，还原到非 C: 目标也正确；含空格必须带引号）。原 `ZJSwapCleanup` 走同助手，行为不变。`ops.h` 的 keepOld 注释同步。
  · **回归**：`make check` 51 用例/366 断言 + check-docs PASS + check-i18n **475/475 不变**（未新增界面字符串）+ check-widths OK + check-shell PASS；`make package` 双架构（`0.7.58`）。
  · ✅ **e2e 首启链实测通过（2026-10-17，QEMU 双系统盘，`0.7.61`，连跑两轮口径一致）**：`inject4 -FirstBoot`（造 fake `~old` + 预置 `D:\ZJRESTORE\{bootfix,rescue-build.txt,logs\zj-demo-migrate.log}` 与 `D:\ZJRESTORE-logs\{ZJRESTORE-last.log,ZJRESTORE-status.txt}` + 写 RunOnce `ZJSysRecoverFirstBoot`）→ 启动新系统 → **产品自动拉起**（zjclick 日志 `t=0s` 已见确认框）→ `15:50:50 clicked DELETE (right btn)` → `t=3s SUCCESS: C:\~old is gone`；同时日志 `15:50:47 log migrate: moved=3 failed=0 sources=2 dest=C:\Program Files\SysRecover\logs`（**早于**确认框 15:50:50 = 顺序正确）；离线取证：`C:\Program Files\SysRecover\logs\` 有 3 个植入文件 + `SysRecover-20261017.log`、`D:\ZJRESTORE-logs` 已消失、`D:\ZJRESTORE` 只剩 `bootfix`/`rescue-build.txt`、`S:\~old` 不存在、无 crash/无 Minidump。
  · 🐞 **本轮实测抓出的真 bug（已修，`0.7.60`/`0.7.61`）**：日志显示 `hide dir ok: D:\ZJRESTORE`，但**盘上 FAT 原始目录项 attr=0x10（只有 DIR，没有 HIDDEN）** —— 根因是 `RemoveEmptyDir()` 删目录前**一律 `SetFileAttributesW(dir, FILE_ATTRIBUTE_NORMAL)`**，而它在迁移里被用于 `base`（`D:\ZJRESTORE`，**非空**、删不掉）→ 刚设好的隐藏位被自己抹掉。**修法**：`RemoveEmptyDir` **只清 READONLY**（`a & ~FILE_ATTRIBUTE_READONLY`，`RemoveDirectoryW` 唯一需要清的位）、保留其它属性；并在 `base` 分支末尾**再 `HideDir` 一次**兜底。修复后同一演练 FAT 目录项 = **`attr=0x12 HIDDEN|DIR`** ✓。同批把 `HideDir` 变成**会留证**（SetFileAttributesW 失败 → `hide dir failed … err=`；写成功但复核不生效 → `hide dir not sticky:`；生效 → `hide dir ok:`）—— 静默不隐藏正是这次差点漏掉的原因。
  · **教训/纪律**：① 改"日志到哪去"这类基础规则时，**先把每条规则的判据抽成纯函数**（本次 `UseInstallDirLogHome`），否则 PE/单分区/只读介质这些分支只能靠现场试；② `//` 注释行**末尾的 `\` 是续行符**（`-Wcomment`）：写 Windows 路径的注释别在行尾留反斜杠；③ 匿名 namespace 里的函数调命名空间内的 `LogWarn` 必须写全 `sysrecover::LogWarn`；④ **"设置属性"类操作不许静默**（`HideDir` 现已三级留证）；⑤ **通用清理函数别乱清属性**（`RemoveEmptyDir` 只该清它真正需要的 READONLY 位 —— 一律清成 NORMAL 会顺带抹掉别人刚设的 HIDDEN/SYSTEM）；⑥ **验证"属性类"需求必须读盘上原始元数据**（PowerShell `Get-Item -Force` 已够，但 FAT 目录项直接读更权威：本轮即靠 `.img` 里 `attr=0x10` vs `0x12` 定案）；⑦ **harness 的 RunOnce 坑**：shell 处理 `RunOnce` 是**顺序 + 同步等每个进程退出**，长驻进程会**队头阻塞**同键后续项（首轮"GUI 未出现"实为自测脚本 `zjclick` 占了 RunOnce，改挂 `Run` 键异步启动即解决 —— **不是产品 bug**）。

### PIT-151

- PIT-151 **客户实测「按 ESC 不是启动菜单」→ 开机键文案不再写死 F12：新增品牌识别模块 `common/bioskey`（~215 条表移植自 BiosKC.c）+ GUI 三处改用识别结果**（2026-10-18，`0.7.62`；客户反馈驱动）：
  · **客户反馈**：HP laptop (redacted)（AMI F.52，UEFI，Win11）按我们提示的 ESC/F12 都到不了启动项列表 —— 客户截图实为 **HP Startup Menu**（只列 F1/F2/F9/F10/F11 入口）。事实：**HP 的启动菜单键是 F9、BIOS 设置是 F10/ESC**（参考表 `BiosKC.c` 原文 `{"HP",…,"F10 or ESC","F9"}`）→ 写死 F12 对 HP 全错。只改文案（第 2 项「装菜单异步化」未获批、暂缓）。
  · **新模块 `src/common/bioskey.{h,cpp}`**：`{kw,vendor,boot}` 表 ~215 条（**表顺序 = 优先级、子串匹配**，与原版 `BiosKC.c::match()` 同口径）；`LookupBootMenuKey(板厂,板型,整机厂,整机名)` 纯逻辑（便于单测）；`LocalBootMenuKey()` 读**注册表** `HKLM\HARDWARE\DESCRIPTION\System\BIOS`（BaseBoardManufacturer/BaseBoardProduct/SystemManufacturer/SystemProductName —— 同 selfdiag 读法，**非 WMI**，§0 红线）；`BootMenuKeyHint()` 展示用（`" or "` → `Tr(" 或 ")`；**拿不准 → `Tr("快捷启动键")` 中性措辞，绝不写死**；static 缓存）。
  · **接入 GUI 三处**（`src/gui/main_form.cpp`）：已安装悬停提示、安装前确认框、安装成功状态栏的 **UEFI 分支**改用 `%ls` + `BootMenuKeyHint()`；BIOS 分支文案本就写「启动菜单里选」不含键名、不变。`ops.h`/`ops.cpp` 注释同步。HP 客户升级后首屏即显示「开机按 **F9** 选」（识别不到才退回「快捷启动键」）。
  · **i18n**：3 个变体键（悬停/确认/状态栏）重生成 + 新键 `快捷启动键`(en: boot menu key)/` 或 `(en: " or ") → `check-i18n` **477/477**；`make check` **52 用例/376 断言**（新增 `bios_key_lookup`：HP→F9、ASUS→F8、Dell→F12、Lenovo→`F12 or Fn+F12`、Surface→空、未知→空、vendor 回填）；check-docs/check-widths/check-shell 全过；`make package` 双架构（`0.7.62`）。
  · **坑（测试输入）**：`strstr` 子串匹配有假阳性 —— 测试想用的"无名词"输入 `SomeNoName` 里含 **`OMEN`**（HP 游戏品牌条目）→ 被识别成 HP。原版 BiosKC 同款行为（表顺序兜底，产品上来源串都是真实厂商/型号名，风险可接受）；**测试用例串要避开表内关键字**（改用 `FooBar Tech`）。

### PIT-152

- PIT-152 **「安装菜单」点击后 GUI "(无响应)"（装/删菜单同步跑在 UI 线程，~28MB 载荷写入 + ~56MB 双读 CRC 校验 → 数秒无消息泵 → Windows 幽灵窗）；修复 = 异步化 + 增量部署（跳过未变化载荷）**（2026-10-18，`0.7.63`；用户实测反馈驱动）：
  · **根因**：`ToggleBootMenu`（`main_form.cpp`）原在 UI 线程直接调 `ops::StageRestoreMenu` —— 它写契约、挂 ESP、`InstallUefiBootEntry` 落 ~28MB 救援载荷（vmlinuz 12,142,528B + initramfs 15,909,740B）并逐文件双读 CRC32 校验（≈56MB I/O）后才回消息泵 → Windows 5 秒无消息即判"(无响应)"。装/删菜单曾是**唯一同步裸奔的写盘路径**（备份/还原早已异步）。
  · **修复 A（异步化）**：与备份/还原同款 —— UI 线程只「定格请求」（镜像/分区/busy 并发保护，`StartMenuToggle`）→ 状态栏文案 + `SetTimer` → worker（BELOW_NORMAL）调 `StageRestoreMenu(req, err, progressFn, &adv)`，进度经 `WM_PROGRESS_UPDATE`（100ms 节流 + 阶段变化直通），完成经新消息 **`WM_MENU_DONE` = WM_APP+19**（worker→主线程，`OnMenuDone`；⚠️ 自定义 WM_APP 必须避让 Duilib 的 `WM_APP+1`，PIT-096）。删除同走 worker（`RemoveBootLayer` + `DeleteMenuBinding`）。`ops.h/ops.cpp` 的 `StageRestoreMenu` 增加 `ProgressFn` 参数（默认 nullptr，CLI 不受影响），引导层前补 `progress(99,"boot")` 收官标记；`StageCn` 增 `"boot"` → 「写入引导文件」。
  · **修复 B（增量部署）**：`grub.cpp` 新增 `CopyIfChanged`（dst 存在且 CRC32 与源相同 → 跳过写盘，日志 `keep ... (unchanged)`；不一致才 `CopyOne`，复制后拿**已算过的源 CRC**只读回目标一次校验 —— 省掉旧 `VerifyCopy` 的重读源；A6 校验语义不变），`CopyBootFiles` 三个文件改走它；`uefi.cpp` `InstallUefiBootEntry` 重构 —— 不再整目录 `RemoveTree` 全清，改为**定点清 stale**（keepNames 白名单 = 本次文件集 + `rescue-build.txt`，其余文件/目录删除，成功/失败均记日志）+ 每文件 **keep/todo** 决策（同 CRC 跳过复制），空间预检 `need` **只累加 todo 文件**（修掉"其实只差几 MB、却按全量 ~49MB 误报空间不足"）；换 Secure Boot 走法时旧文件集仍被清掉（防"越装越多把 ESP 塞满"）。
  · **效果**：重复安装（载荷未变）几乎零拷贝、秒级完成；首装/换版仍全量写 + CRC 校验；GUI 全程有进度与「已用」刷新，不再幽灵窗。
  · **回归**：`make check` 52 用例/376 断言 + check-docs PASS + check-i18n **479/479**（新增 `正在删除启动还原菜单...`、`写入引导文件` 两键）+ check-shell PASS；`make package` 双架构（`0.7.63`）。🚧 待用户真机复测：① 装菜单时 GUI 不再"(无响应)"、进度条与「已用」走动；② 重复安装第二次日志出现 `keep ... (unchanged)` 且明显更快；③ 删除菜单同样不卡。

### PIT-153

- PIT-153 **Q2：PE/U盘就地还原不再限制 BIOS/UEFI 装法（删 GPT 硬门禁）+ 暂存路径新增会话前置检查（两种必坏组合提前拒绝）**（2026-10-18，`0.7.64`；用户裁定两项）：
  · **背景**：`safety.cpp::CheckRestoreTarget` 原有「GPT 目标 + 非 UEFI 固件 → 硬拒绝」门禁（PIT-034/092 时代产物），导致 PE（常为 Legacy 引导）里就地还原 GPT 盘直接失败。用户裁定：就地还原任何组合全放行（bcdboot/ESP 子镜像为纯文件 I/O，任意会话可跑）；「暂存+重启」依赖重启后固件沿**当前同一种方式**引导，两种组合必坏，提前拒绝并指路 PE。
  · **删门禁**：`safety.cpp` 移除 GPT-BIOS 硬拒分支，注释指向新检查；`safety.h` 同步。
  · **新纯逻辑** `src/boot/bootpath.{h,cpp}`：`enum class StagedBootPath { Ok, BiosSessionGptTarget, UefiSessionMbrTarget }` + `CheckStagedBootPath(sessionIsUefi, diskStyle)` —— 返回码（零 i18n 依赖，保持 bootpath 可单测）；中文提示由调用方按码组 `Tr()`。
  · **调用点**（`ops.cpp`）：① `StageRestoreImpl` 在 `inPlace` 判定之后、A 引擎解压 `~new` **之前**（覆盖 staged restore + menuEntry，就地路径不检查）；② `StageReturnOld` 前置检查后（returnold 专用文案，不照搬「请就地还原」）。
  · **文案**（i18n 4 键）：BIOS会话+GPT目标 →「请改用 UEFI 模式的 PE/U盘启动后就地还原（无需重启）」；UEFI会话+MBR目标 → 对称（Legacy/CSM）。
  · **单测**：`TEST(check_staged_boot_path)` 6 断言（两拒 + GPT+UEFI/MBR+BIOS/Unknown×2 放行）；`TEST(boot_path_choice)` 注释更新（safety 不再预拒 GPT+BIOS）。
  · **回归**：`make check` **53 用例/382 断言** + check-i18n **482/482** + check-docs PASS；`make package` 双架构（`0.7.64`）。🚧 待实测：PE（Legacy）就地还原 GPT 盘不再被拒；BIOS 会话暂存 GPT 目标 / UEFI 会话暂存 MBR 目标得到明确中文拒绝。
  · **第二批（2026-10-18，`0.7.65`）：ESP 一律限定「目标盘」——多盘机防写错盘**：
    ① **隐患**：`AcquireEspRoot` 原先无条件 ① `MountEsp`（`mountvol /s` 挂**固件真 ESP**）——多盘机固件 ESP 常在**别的盘**，后续 bcdboot/救援载荷就写到错盘（用户 Q2 批准的决策：挂上的 ESP 必须落在目标盘）。
    ② **双端限定**：`FindEspPartition`/`FindEspPartitionFallback` 加 `int onlyDiskIndex = -1` 参数（`d.index != onlyDiskIndex` 跳过）；ops.cpp **5 个调用点**（就地 ESP 恢复/就地修引导/暂存/returnold/repair-boot）传 `target.diskIndex`，**备份 esp merge 保持 -1**（本机 ESP，行为与改前一致）。
    ③ **挂载后盘号校验**：`VolumeDiskIndex(letter)`（`\\.\X:` + `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS` → `Extents[0].DiskNumber`，失败 -1）；错盘 → log + `UnmountEsp` + **手动 `espRoot.clear()`**（`UnmountEsp` 是 const 引用、**不清字符串**）后续查 ②③；**取不到(-1) → fail-open 保留挂载 + WARN**（ioctl 失败极罕见，不能因它卡死合法还原）；②③ 都失败 → **绝不回退错盘**、报错带 `DescribePartitions` 摘要。
    ④ **②b 新路 `MountEspByGuid`**：目标盘 ESP **无盘符**时按卷 GUID 直接挂（`mountvol X: \\?\Volume{GUID}`；GPT 卷 GUID=分区 GUID，`GuidToString` 小写无花括号、代码补花括号并转大写求稳；S..Z 空闲符，挂上却看不到内容则 `/d` 撤销换下一个）。**没有 ②b 必硬失败**：① 被盘号拒、③ 又要求有盘符。
    ⑤ **`RemoveBootLayer` 去 `IsUefiFirmware()` 门控**：总是试挂 ESP 清 `\EFI\ZJRESTORE`——固件类型可能漏报（PIT-092 同族），漏清=残留；纯 BIOS 挂不上即跳过，只多几条日志、零副作用。
    ⑥ ⚠️ **本次踩坑（两教训）**：a) 手写字符字面量丢收尾引号（`L'A)))` 应为 `L'A')))`）→ GCC 连环 `expected ')'`，而 **`make check` 不编译 ops.o**（单测只链纯逻辑 TU）→ check 全绿也拦不住，**最终判据必须是 `make package` 的编译**；b) 行号/行数比对**别用 PS `Get-Content`**（同文件行数与编辑器不一致 2433 vs 2706），用 `[System.IO.File]::ReadAllLines` + 码位校对（本轮靠 `76,39,65,41` vs `76,39,65,39,41` 一眼定罪）。
    ⑦ 回归：`make check` **53 用例/382 断言** + i18n **482/482** + `make package` 双架构（**`0.7.65`**）。🚧 待实测：多盘机暂存/就地还原，日志应出现 `mounted ESP is on disk N but target is disk M; unmount...`（错盘被拒）或 `ESP mounted by volume GUID at X:`（②b 生效）。

### PIT-154

- PIT-154 **Q1「先落盘后搬家」（SettleHome）：程序目录在目标盘上时先静默复制整包+日志到安全分区，点确认后再搬家**（2026-10-18，`0.7.66`；用户三项确认：①启动搬迁块删除 ②SettleHome=复制不re-exec ③仅还原/returnold触发、物理盘比较）：
  · **架构**：`SettleHome(targetDiskIndex, exeDiskIndex)`（ops.cpp，common不依赖disk）；复制用ops自有`CopyDir`；空间检查`AppTreeSize()+300MB`；目的地=`<safe>:\ZJRESTORE`（`kRecoveryDir`）；`SelectRelocateDrive`加`avoidTarget`参数（跳过目标盘符）；幂等=进程内static `g_settleResult.checked`标志。
  · **日志根覆盖**：`SetLogHomeOverride(dir)`+`ContractDir()`（override空则AppDir否则override）；`LogBaseDir()`顶部优先返回override；`StageRestoreImpl`/`StageReturnOld`的`exeDir`改用`ContractDir()`；契约写者/读者（`WriteRestoreTask`/`ReadMenuBinding`/`DeleteMenuBinding`/`RetryStagedRestore`）统一走`ContractDir()`。
  · **FindDataDrive加avoidLetter**：还原时排除目标盘符，防契约写进会被格式化的盘；6个调用点传`target.letter[0]`。
  · **删除启动搬迁块**：GUI `main_win.cpp`（保留A引擎残留分流）、CLI `main.cpp`（保留`CheckLastRescueFailure`等启动逻辑）；等用户点「开始还原」确认后，由`StageRestoreImpl`自动调用`SettleHome`。
  · **警告（fail-safe）**：无安全盘→`g_settleResult.noSafeDrive=true`；CLI在`CmdRestore`末尾stderr打印；GUI在`OnTaskComplete`弹MessageBox（静默模式也弹）；文案i18n：「警告：程序目录在目标盘上且无法安全搬迁——还原后程序可能丢失。」
  · **单测**：`TEST(relocate_drive_choice)`扩4断言（avoidTarget跳过、同盘、唯一候选被排、大小写）→ **53用例/386断言**。
  · **坑**：多行注释**行尾不能有 `\`**（会吞下一行声明，GCC `-Wcomment`警告；本次`// 复制整包+日志到<chosen>:\ZJRESTORE\`行尾\导致`destRoot`未声明）；**`make check`不编译ops.o**（单测只链纯逻辑TU）→ check全绿也拦不住ops.cpp编译错，**最终判据必须是`make package`的编译**。
  · **回归**：`make check` **53用例/386断言**+check-i18n **471/471**+check-docs PASS+check-shell PASS；`make package`双架构（**`0.7.66`**）。🚧 待实测：程序在目标盘上点「开始还原」→确认后自动复制到安全盘→还原成功后新家有完整程序+日志；无安全盘时弹警告框。

### PIT-155

- PIT-155 **Q1 SettleHome 三处加固：①触发判据抽纯函数+单测 ②探针残留清理 ③StageReturnOld 补 SettleHome**（2026-10-18，`0.7.67`；承 PIT-154，核查中发现三个问题逐一修）：
  · **① 触发判据抽纯函数**：`ShouldSettleHome(exeDir, targetLetter)`（relocate.{h,cpp}，可单测）——还原只格式化目标分区，同盘不同分区程序不受影响，仅同盘符才触发；ops.cpp 触发点（StageRestoreImpl :1431 + StageReturnOld :2259）改调它（原先是手写 `SameDrive` 内联，逻辑正确但不可测）。**触发面偏窄问题经复核不成立**：还原只格式化目标分区，同盘不同分区的程序不受影响，无需搬迁——盘符门禁即正确粒度。
  · **② 探针残留清理**：SettleHome 快速可写探针在每个候选盘建 `ZJRESTORE` 目录，未选中的留下空目录（`RemoveDirectoryW` 只删空目录，有内容静默失败）——修后选完候选即清未选中探针目录，避免在无关盘上留残迹。
  · **③ StageReturnOld 补 SettleHome**：原实现程序在目标分区时直接前置拒绝 return 4（让用户手动拷到别的分区）——违反「还原/returnold 都触发 SettleHome」规格；修后先调 `SettleHome`，搬成功则 `exeDir = ContractDir()` 指向新家继续，搬不动才拒绝（fail-safe 保留）。
  · **单测**：新增 `TEST(settle_home_trigger)`（relocate.cpp 已在 TEST_UNITS）——同盘符/大小写/异盘/targetLetter=0/空路径/太短/盘符根 7 类场景 → `make check` **54用例/396断言**。⚠️ 边界坑：`L"C:"`（尺寸2、有冒号）应**触发**（原断言写反 CHECK(!...)，首跑 FAIL 后改正）。
  · **回归**：`make check` **54用例/396断言**+check-i18n **471/471**+check-docs PASS+check-widths OK+check-shell PASS；`make package`双架构（**`0.7.67`**）。🚧 待实测：程序在目标盘上点「开始还原」/「返回旧系统」→确认后自动复制到安全盘→操作照常继续；多盘机探针后无关盘上不再残留空 `ZJRESTORE` 目录。

### PIT-156

- PIT-156 **还原残留归零：成功还原后 `C:\ZJRESTORE`（~496KB EA 载荷）永久残留 → 救援写来源标记 + 补写器首启归纳 + 每次启动归纳 + 分类器整体清除**（2026-10-19，`0.7.68`；用户 2026-10-18 拍板三项）：
  · **铁证与病灶**：EA 链（PIT-122）救援投 `C:\ZJRESTORE\ea\{zj-ea-apply.exe,…}`（`zj-ea-apply.exe` = 507,392B）→ 补写器自清理**只延迟删自身 exe/cmd**、Windows 启动链（migrate/collect/cleanup）**只收不删文件夹** → 成功还原后 `C:\ZJRESTORE` 永驻（boot/scripts/bootfix 还原后都不留，唯一在目标盘重建它的**成功**路径 = EA 块）。
  · **用户拍板三项**：① 移除触发 = 补写器首启自动归纳 + 软件以后**每次启动**兜底；② 收集/归纳后**整个删掉**残留文件夹（不逐文件）；③ `D:\ZJRESTORE` 同样归纳并清掉（推广 `MigrateLogsToLogHome`，加「暂存待还原期间不搬」守卫）。
  · **启动链顺序（不可反）**：migrate → collect → cleanup —— 清理器会删源/搬空源，迁移必须先行。
  · **救援层（`zjrestore-lite.sh`）**：EA 块 mkdir 成功后写 `$EAM/ZJRESTORE/source.txt`（键：time/engine/action/applier/software_dir/software_path/software_version/rescue_build/target_*/image_path）；LF 换行；部分部署/后续失败也留证据；写失败 `WARN` 不阻断。
  · **首启补写器（`ea_apply_main.cpp`）**：新增 `GatherResidueToSoftwareDir(root)` —— 读 `source.txt` → `software_dir`（空则从 `software_path` 去末段）→ 归纳到 `<soft>\logs\collected\ZJRESTORE-ea\`（固定名覆盖写防重试增殖；拷 source.txt + `ea\` 下文本，跳 `*.exe`/`*.dat` 与 >8MB；`CopyFileRaw` 先清属性再拷 —— PIT-067/068 教训）；**不删 ZJRESTORE 文件夹**（菜单 boot/scripts 会被误删，删除留给 Windows 侧分类器）；`--no-cleanup` 也执行；调用点 = 清理块之前、无论 rc 都跑。
  · **分类器（`relocate.{h,cpp}::ClassifyZjrResidue`，纯逻辑+单测）**：Keep = 含 `sysrecover.exe`/`sysrecoverui.exe`（SettleHome 程序的家）或顶层 `boot`/`scripts` **精确名**（已装启动菜单载荷；bootfix/bootfiles 不命中）；含我方信号 {`ea`,`logs`,`bootfix`,`source.txt`,`rescue-build.txt`} → **Delete**（含杂物一并整删，因整个文件夹是还原期重建）；空 → Empty；其余 → Keep（看不懂就留）。
  · **`MigrateLogsToLogHome` 推广**：去 `UseInstallDirLogHome` 门控改为每次启动调；守卫 = `IsRunningInPe()` → 空、dest 有 `pending-restore.txt` → 跳过（暂存待还原不搬）；源 == activeBase（`_wcsicmp`；`AppDir()` 无尾斜杠已核）跳过自搬；保留 `ZJRESTORE-logs` 兜底窝迁移/HideDir/搬空删壳；无源时返回空串（不再每次刷日志）。GUI/CLI 启动链统一接入（`main_form.cpp` 无条件调用、`cli/main.cpp` 先于 collect/cleanup）。
  · **`CleanupStrayLogs` 升级**：枚举盘根 `ZJRESTORE` 顶层条目 → 分类器 switch：Delete → `RemoveTreeAny`+LogInfo；Empty → 清属性删空壳；Keep → 不动（活动日志根 = 分类器的家，天然 Keep）。收集器 `CollectOneDrive` 加收 `source.txt`（与 `rescue-build.txt` 并列）。
  · **回归**：`make check` **55 用例/411 断言**（新增 `TEST(zjr_residue_classify)`）+ check-i18n 471/471 + check-docs + check-widths + check-shell PASS；`make package` 双架构（**`0.7.68`**，applier 507,392→518,144B；initramfs 重建 stamp 0.7.68）。check-shell 基线 160→161（新增 1 处 `cat /zjrescue-version 2>/dev/null` 属 A 类探针读，与既有 :367 同款；原 `> source.txt 2>/dev/null` 已改留 shell 原生错误 + `|| say WARN` 响亮兜底，-1）。e2e 演练台预期同步更新（`inject4/inspect1`：PIT-150「ZJRESTORE 残留 hidden」已被本规格取代为「应整体消失」；`inspect3` 的菜单载荷 `boot` 断言不受影响，Keep 规则兜住）。
  · **教训**：① 残留清理必须**先收集再删**（证据优先）；② 删除粒度 = 整个文件夹时，"这个文件夹是谁的"必须先分类（程序的家/已装菜单 vs 还原期残留），**看不懂的一律留**；③ 让**源头写来源标记**（`source.txt`）比事后猜归属可靠 —— 现场日志要能回答"这是谁放的"。

### PIT-157

- PIT-157 **1019 案「PIT-135 补课」：非微软重解析点致 Linux apply rc=58 的双端加固（F1~F6）——谓词与上游逐行同构、备份端捕获 + 就地还原补写、block 暂存预检"动盘前"拒绝**（2026-10-19，`0.7.69`）：
  · **案发**：Dell ChengMing 3900（= PIT-135 同机）用旧镜像再次 rc=58（`ipfsrv.dptf`，`Invalid argument`）→ mkntfs 后 apply 中止、目标半成品。0.6.51 备份端排除 + 0.6.52 ZJEA 链此前**从未真机验证**；且 F1~F3 三处实现缺陷使其形同虚设——链路不复核即等于没有。
  · **F1 谓词重写**（`ea.cpp::IsUnsettableReparse` :439）：从"只查 bit31"改为**逐行对齐 ntfs-3g `valid_reparse_data` + wimlib `complete_reparse_point`** 的全结构校验——非MS（tag bit31=0）→拒（PIT-135 语义保留）；无 blob 时仅 `MOUNT_POINT 且非目录`→拒；tag==0→拒；长度方程 `datalen == u16(4)+8`；子类型：MOUNT_POINT（<16/越界）、SYMLINK（<20/越界）、LX_SYMLINK（`u32(8)==2`）、0x80000023-26（需 RECALL_ON_OPEN）。**上游会拒的形状我们必拒**（客户真实 tag 未取得也能覆盖全部 bit31=1 静默分支）。`IsNonMsReparseTag` 保留，`NormalizeReparseBlob` 对非MS 按 GUID 式重算 datalen（失败不改缓冲）。
  · **F2 捕获端**（`CaptureReparse` 重写 + `FindUnsettableReparse` 新增 :534）：门控 `!bad && !ReparseCheckNeedsBlob(tag)` 才跳过；有 blob 才读文件归一化后判定（不读未命名流）；cap 64；返回条数/-1（err 可读）。
  · **F3 回放端**（`ApplyRepack` 加固）：目录条目（`FILE_ATTRIBUTE_DIRECTORY`）走 `CreateDirectoryW`；SET 前按 wimlib 算法归一化 datalen；SET/mtime 打开 `FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS`；FAILED 日志补 `(err=N)`（现场可评估）。
  · **F4 就地还原补写**（PE / 非系统盘）：`RunDirectRestore` 增 `eaIndex` —— 主镜像 Apply 后从 ZJEA 子镜像 extract 到 `%TEMP%\SysRecoverEA-*`，**先 ApplyRepack（重解析点）再 ApplyPack（EA）** 到目标根；失败仅 WARN 非致命；提取后清临时目录。
  · **F5 block 暂存预检**（fail-closed）：`StageRestoreImpl` Step 0.8（winload 预检后、一切写盘前）：`engine=block` 且镜像含不可设重解析点（`wim::FindUnsettableReparseInImage`，iterate_dir_tree 只读元数据）→ **rc=4 拒绝** + 指引（PE 就地还原 / 新版重备份）；swap 不查（目录交换不受影响）。**旧镜像在格式化之前就被拦下**。
  · **F6 回归资产**：① 单测 `ea_unsettable_reparse` 全分支覆盖（`raw()` 造 blob；56 用例/433 断言）；② lab 工具 `tools/reparse-lab.cpp`（make/check/roundtrip，**不入产品、ad hoc 编译**）——宿主实测 PASS：GUID 形态 SET/GET（tag 0xDEAD）→ 谓词三口径全 UNSETTABLE → `CaptureReparse→删→ApplyRepack` 后 **blob 43B / content 39B 逐字节一致**（**repack 回放首次真机验证**）；③ 0x0/0x1 是 `IO_REPARSE_TAG_RESERVED_*`（SET 报 `err=4393`，lab 默认 0xDEAD）。
  · **教训**：① "只查位标志"的快筛与上游真实拒绝面差一大截——移植上游校验必须逐行落齐；② "排除+记录+回放"链每一环都要有单测/实验台，否则长期未验证 = 形同虚设（本链 0.6.51→0.7.68 从未真机跑过）；③ 防御闸门设在代价最小处：F5 把拒绝提到格式化之前，用户零损失。
  · 回归：`make check` 56 用例/433 断言 + check-docs/i18n(473)/widths/shell PASS；`make package` 双架构（`0.7.69`）。
  · **W1 救援层动盘前预检（同日追加，`0.7.70`）——"旧镜像走救援层"通道，动盘前拦下而不是格完盘才发现**：F5 只管**暂存**路径；老镜像（契约无证书）在 Linux 侧于 mkntfs 之前做同款检查：
    - **判据**：`reparse_scan=0` 且 `image_size` == 镜像实际字节数（Windows 侧出具的 fast-path 证书，秒级）→ 直接放行；否则 `wimlib-imagex dir --detailed`（timeout 180）逐行 awk —— `Reparse Tag = 0x...` 去 `0x` 后**长度<8 或（=8 且首字符 0-7）→ 计坏**（bit31=0 非微软；MS 标签如 0x8000001a/0xa0000003 不误报——合成测试 2 保留/5 标记）。BAD>0 → 拒绝；DIRRC≠0 或 SEEN 空（解析不了）→ **fail-closed 拒绝**。busybox 无 PIPESTATUS → `echo DIRRC=$? >&2` + sed 提取。
    - **契约**：增量键 `image_size`（十进制字节）/`reparse_scan`（0=证书；-1/2=无证书），不升 contract_version；Windows 侧仅在 block 引擎且 F1 谓词扫描干净时写证书（`GetFileAttributesExW` 取实际大小）。
    - **拒绝行为**：`RESULT=FAILED; STEP=reparse-unsettable; exit 1` → STAGE=0 ⇒ bb_final 自动重启回 Windows（`recover decision: reboot (target untouched, STEP=reparse-unsettable)`）；C 类、不提供一键重试；GUI 回执中文化（selfdiag `镜像含无法还原的重解析点`）；救援横幅指引旧镜像用户改走 A 引擎暂存（bootfix 中立格式）或 PE 就地还原。
    - **QEMU 演练 4 例全过**：cert 11/11（快路径、无 metadata scan）、默认 scan 10/10、stale 11/11（image_size+1 → 尺寸不符强制全扫）、reject 10/10（`bad tag 0xdead` 列出、**mkntfs/apply 零发生**（负面断言）、rc=1、自动重启决策在案）。载体 = `tools/reparse-lab.cpp` 新 `capture` 模式（**无排除规则** libwim 捕获，模拟"老软件产物"含 GUID 0xDEAD 的 `reparse-test.wim`；宿主 capture→extract 往返 tag 保留、谓词 UNSETTABLE）+ mk-drill `ZJ_DRILL_REPARSE=cert|stale|reject` + run-drill `ZJ_EXPECT_REPARSE`（含 mkntfs/apply 不得出现的负面断言）+ `tools/vmtest/test-reparse-awk.sh` 合成分类测试。
    - 回归：`make check` **57 用例/446 断言** + check-docs/i18n(474)/widths/shell PASS；`make package` 双架构（`0.7.70`）。
    - **W2 EFS 加密内容（同日追加，`0.7.71`）——"EFS 文件被静默还原成空文件"的双端拦截**：与重解析点同族的第二类 Linux 还原不了的内容：**EFS 加密文件**。实测形态（宿主 + QEMU 演练双确认）：**Linux 侧 apply `rc=0`、wimlib 只打 `[WARNING] Ignoring EFS-encrypted data of 1 files`、文件落地 0 字节**（静默数据丢失，比 rc=58 更隐蔽）；**Windows 侧同镜像 extract 完整往返**（文件保持 Encrypted、解密后 SHA256 一致）⇒ 与 F5 同款结论：**block 引擎必须拒绝，A 引擎/PE 就地还原安全**。
      - **检测**：元数据扫描合并进 F5 同一次遍历（`wim::FindUnsettableReparseInImage` → 改名 **`ScanImageForLinuxRestore`**，加 `encryptedCount` 出参；dentry `FILE_ATTRIBUTE_ENCRYPTED` 只读元数据）；救援侧复用 W1 同一次 `dir --detailed` awk —— imagex.c:2602-2615 的属性行 `    FILE_ATTRIBUTE_ENCRYPTED is set` 计数（与所带 wimlib 1.14.4 源码逐字对齐）。
      - **契约**：新增增量键 **`efs_scan`**（0=干净/2=不可用/-1=未扫描；语义同 `reparse_scan`）。**救援层 fast path 改为要求 `reparse_scan=0` 且 `efs_scan=0` 且 `image_size` 匹配** —— 0.7.70 版 exe 的旧证书只有 reparse 键，缺 `efs_scan` 时强制全扫描（**版本偏斜防护**，否则旧暂存+新救援会漏掉 EFS）。
      - **拒绝行为**：Windows 暂存（block）`rc=4` + 中文指引（改选「还原后重启」或 PE 就地还原）；救援层 `RESULT=FAILED; STEP=efs-encrypted`（C 类不可重试、selfdiag 回执「镜像含 EFS 加密文件（还原后会变成空文件）」）+ STAGE=0 自动重启回 Windows；均**未动目标分区**。
      - **验证**：① 宿主实验（`cipher /e` 造密 → `reparse-lab capture` → 新 `dir` 模式验 ENCRYPTED 属性 + EFSRPC 原始数据流在镜像里；Windows exe extract 对照哈希一致）；② **观察轮**（W2 前代码）：apply rc=0 + warning + `efs1.bin` 盘上 **0 字节**（7z 读 NTFS 实证）；③ QEMU **拒绝轮**：`encrypted entries: 1`、**mkntfs/apply 零发生**（负面断言）、rc=1、自动重启（`ZJ_DRILL_EFS=1` + `ZJ_EXPECT_EFS=reject`，10/10）；④ **回归全套**：block 默认 10/10（全扫描+ok）、cert 11/11（**双键**快路径）、stale 11/11、**oldcert 12/12**（新增变体：只有 reparse_scan 的旧证书 → 强制全扫描、`mustNot certified clean`）、reparse-reject 10/10、swap 13/13；CLI 实机冒烟：EFS+block → rc=4 中文拒绝、EFS+swap → 不拦截（走到目标查找）、reparse 图 → rc=4（F5 回归）、干净图 → 无误报。
      - **教训**：① "静默成功但数据没了"比"响亮失败"更危险 —— rc=0 路径也要过一遍"目标盘上实际落了什么"（本次靠 7z 读回实证）；② 证书类增量键升级时要检查**每个键都已写入才走快路径**（缺一个键 = 旧版本证书，必须退回全扫）；③ 同名具的不同危害可共用一次扫描（重解析点 + EFS 同趟完成，零额外开销）。
      - 回归：`make check` **57 用例/453 断言**（+7：三 builder 的 efs_scan、默认值、TaskConfGet、RescueStepRetryable、日志）+ check-docs/i18n(**477**)/widths/shell PASS；`make package` 双架构（**`0.7.71`**，含 initramfs 重建 stamp 0.7.71）。
    - **W3 提示文案前移（同日追加，`0.7.72`）——"选择时刻"就告诉用户该选哪个引擎**：W1/W2 的拒绝发生在选定引擎之后（弹错误框让用户重来），W3 把提示前移到三个面：
      - **GUI 三选确认框扩到 4 行**（`CConfirmDlg`）：新增 `DlgMsg4`（skin/confirm.xml，y122）+ `MsgLineCount()` 动态加高 22px、按钮同步下移；**≤3 行的所有既有对话框（Ask2 等）尺寸/位置完全不变**（`extra=0` 不动布局）；第 4 行文案「含 EFS 加密文件或特殊重解析点的镜像，请选「还原后重启」」。
      - **CLI**：`restore --engine block` 暂存前打印同款提示（中/英；swap 不打印）。
      - **救援横幅**：沿用 W1/W2 已实现的英文拒绝指引（`use 'restore then reboot' ... or in-place restore from PE`），无需改动。
      - 验证：i18n **478/478**（三端词条：对话框 4 行整串、CLI 提示）；CLI 冒烟 zh/en 均先出提示再 EFS 拒绝（rc=4）、`--engine swap` 无提示；`make check`/`make package` 绿（`0.7.72`）。🚧 4 行对话框的**外观**待 GUI 实测（逻辑：4 行时窗口 196→218、按钮 y140→162）。
    - **W4 init 失败菜单全做（同日追加，`0.7.73`）——"永不给裸 `#`"的最后一块**：`bootfiles/alpine/init` 的 `menu_loop` 重写：① **取消 3 轮上限**（原来第 3 轮后直接 `exec /bin/sh`，等于又把用户扔回裸环境）；② **超时 120s 无输入 = 自动 [2] 重启**（原文提示 `auto-[2]` 与行为一致；旧实现超时走 `break` → 掉 shell，无人值守机器会永远干等）；③ **每轮重印 `RESULT: restore FAILED (rc=… STEP=…)`**（从 `/tmp/zjrestore.log` 取最后一条，屏幕滚动后仍能拍照留证）；④ **`#` 仅经 [3] 显式选择**（唯一 break 出口）；⑤ 非法输入 → 重新提示（不再一敲错就掉 shell）。
      - **测试**：新增 `tools/vmtest/test-menu-loop.sh`（仿 test-caps-failpath：抽 `menu_loop()` + sed 注入可测点 `ZJ_MENU_TMO/ZJ_MENU_LOG/ZJ_STUB`；**14 断言 PASS**：超时→重启、非法输入重提示、retry 超 3 轮（round 4/5 在案）、RESULT/STEP/rc 重印、[3] 才出 shell）；已挂进 `check-shell.py` 的 `SYNTAX_ONLY`（连同 test-reparse-awk.sh，syntax 7→9）。
      - **门禁基线（A/B 分类）**：init 新增 **2 处 `2>/dev/null`**（菜单里从日志取 rc/STEP 的两条 `sed` 读）——**A 类**：日志可能不存在，失败=空输出，调用方有 `${var:-?}`/`${var:-unknown}` 兜底，不是吞真故障 → 按纪律 `--update-baseline`（init 58→**60**），已在本次记录分类理由。
      - **回归**：`make check`（57 用例/453 断言 + i18n 478 + shell PASS syntax 9）+ `make package`（**`0.7.73`**，initramfs 重建 stamp 0.7.73）；演练：**autorrecover 8/8**（STAGE=0 路径无菜单、自动重启）、**block 10/10**、**swap 13/13**、**EFS 拒绝 10/10**（最终 initramfs）。

### PIT-158

- PIT-158 **P4 用户试用反馈三项（`0.7.74`，2026-10-20）：空间不足前置禁用、删除旧系统后自删、目标盘 ZJRESTORE 成功即清**：
  · **① 空间预检前移到确认框**（用户规格："开始前直接显示需要 X GB / 可用 Y GB；够就不提示；不够才把「还原后重启」变成红色「空间不足」且不可点"）：`CConfirmDlg::Ask3` 新增 `midDisabled`（红底红字、忽略点击，回车不落禁用项）；`MsgLineCount` 上限 4→5 并新增 `DlgMsg5`；ops 新增 **`QuerySwapSpace()`**（`ImageSize` + `EstimateSwapSpace`，GUI 在弹框前调用）；`StartRestore` 仅在 `freeBytes>0 && !enough` 时追加第 5 行并 `defaultIdx=0`（回车=取消，避免默认落到"会格盘"的 B 引擎）。
  · **② 删除旧系统后自删**：`CConfirmDlg::Ask4`（四按钮、窗口运行时加宽到 520，顶栏/关闭/消息标签同步重排）；启动期备返对话框由 Ask3 改 Ask4（保留 / 返回旧系统 / 删除旧系统 / **删除旧系统后自删**，默认仍是"保留不动"）。选中后二次确认（默认「否」）→ ①删 ~old（进度窗）→ ②`RemoveBootLayer` + `DeleteMenuBinding` → ③**分离 cmd 自删程序目录**（`ping -n 5` 等本进程退出后 `rmdir /s /q` 重试 30 次；失败只记日志）。**保护**：旧系统没删干净、或启动菜单没清成功 → **取消自删**（不留下"程序没了、引导还坏着"的半成品）。
  · **③ 目标盘 ZJRESTORE 成功即清**：`zj-ea-apply.exe` 在 EA 补写 **rc=0** 时，先把诊断归集到 `<软件目录>\logs\collected\ZJRESTORE-ea`（沿用 PIT-156），再**整目录删除**（分离 cmd 等补写器退出后 rmdir）。**保护**：目录里有 `SysRecover(.UI).exe`（SettleHome 程序的家）或 `boot/scripts`（已装启动菜单）→ 不动；失败（rc≠0）保留供重试；`--no-cleanup` 跳过。
  · **验证**：`make check`（57 用例/453 断言 + i18n **491** + docs/widths/shell 全过）+ `make package` 双架构（**`0.7.74`**，`bootfiles/zj-ea-apply.exe` 529,408B）；自删 cmd 语法在 `%TEMP%` 沙箱实测可删空目录；🚧 **待实测**：① 4/5 行 + 四按钮对话框外观（≤3 行/三选尺寸不变）；② 空间不足时按钮禁用与回车行为；③ 真实首启 EA 后 `C:\ZJRESTORE` 立即消失；④ 真机"删除旧系统后自删"全链（删旧→删菜单→程序退出并自删）。
  · **教训**：① 自删必须"分离 cmd + 等待 + 重试"——运行中的 exe/DLL 无法当场删；② 破坏性选项（自删）要有二次确认与"前置步骤失败就中止"的护栏；③ 用户可见的"空间够不够"必须用**可用空间**（A 引擎）而不是分区总大小（B 引擎才看总容量），两个引擎口径不同。

### PIT-159

- PIT-159 **新硬盘初始化向导 + 第三步启动方式提示 + GUI「修复引导」（`0.7.75`，2026-10-20/21；PE 首次在新机器上运行的需求）**：
  · **背景**：PE 里遇到"没有任何分区的全新硬盘"时，第二步分区下拉为空，用户无从下手；且"当前启动方式 vs 目标盘风格"不一致（Legacy 会话 + GPT / UEFI 会话 + MBR）会导致"还原成功但起不来"，需要**选择时刻**提示。
  · **① 新硬盘初始化（`src/disk/partinit.{h,cpp}`）**：`FindEmptyDisks()` 检测固定盘（排除可移动/U 盘/PE 盘；`EnumerateDisks` 对 RAW 盘会整体跳过 → 单独读 LBA0 探测）；方案：GPT（ESP 300MB + MSR 16MB + 系统盘默认 100GB + 其余数据）或 MBR（系统盘默认 60GB + 其余数据），容量可改。**硬限制**（技术不可能才禁）：>2TB 只能 GPT；Win7 x86/XP 只能 MBR；其余仅推荐 + 警告（**不剥夺用户选择**）。MBR 方案额外 **`bootsect /nt60 <系统盘>: /mbr` 写磁盘引导代码 + diskpart `active`**（新 MBR 盘不写 MBR 代码 → 还原完黑屏）。执行：diskpart 脚本（纯 ASCII）+ 回读校验（风格/分区数/ESP/MBR 引导代码）。
  · **② 入口（用户拍板：不加按钮、不多一步）**：第二步分区下拉改自绘 `PartitionCombo`（`CPartitionComboUI`）——列表为空时点击**不再展开空下拉**，直接 `PostMessage(WM_SKIN_EMPTY_PARTITION)` → 打开向导；非空时行为不变。空列表原因区分：空盘 → 向导；磁盘未联机/BitLocker/无固定盘 → 明确提示（不弹向导）；多块空盘 → 提示只保留目标盘（MVP，避免选错盘）。
  · **③ 启动方式提示**：新增 `disk::IsUefiBootSession()`（读固件变量判断**本次会话**；`IsUefiFirmware` 只是固件能力，Legacy 会话下仍可能是 UEFI 固件）；第三步确认框在"会话 vs 目标风格"不一致时追加一行（**只提示不阻止**）：Legacy+GPT → 还原后改 UEFI 启动、缺启动项用「修复引导」；UEFI+MBR → 改 Legacy/CSM。`CConfirmDlg` 消息上限 5→6 行（`DlgMsg6`）。
  · **④ GUI「修复引导」**：新增 `FixBootBtn`（等价 CLI `repair-boot`），位置在「日志」「清除引导」之前（还原模式：静默 250..350 / 修复 356..438 / 日志 444..502 / 清除 508..590 / 菜单 596..684；备份模式不变、按钮隐藏）；worker 异步 + 结果框。
  · **⑤ i18n 纪律**：新文件 `src/disk/partinit.cpp`、`src/gui/initdisk_dlg.cpp` 已加入 `tools/i18n-wrap.py` 的 `WRAP_FILES`（否则 skeleton 扫不到、还会把词条当死键删掉 —— PIT-109 教训）。
  · **验证**：`make check`（57 用例/453 断言 + i18n **536** + docs/widths/shell 全过）+ `make package` 双架构 + initramfs 重建（stamp 0.7.75）（**`0.7.75`**）。🚧 **待实测**：① 真机/VM 空盘：向导 → 分区 → 还原 → 启动（BIOS 与 UEFI 各一次，验证 MBR 引导代码与 ESP）；② 向导对话框外观与容量校验；③ 空下拉点击/空因提示；④ 第三步提示与「修复引导」按钮；⑤ PE 里 `bootsect.exe` 是否可用（缺失时 MBR 方案会明确失败，需随包补资产或提示改 GPT）。
  · **VM 反馈修正（2026-10-21，`0.7.76`）**：① 向导窗口 **400→430px**（原高度按钮被裁）；② **小容量磁盘规则**（用户规格）：`PartSizeHint SuggestPartitionSizes()` 纯逻辑 —— Win10/11 可用 ≥105GB → 100+数据、65~104 → 60+数据、**<65 → 整盘做系统盘（不建数据盘）**；Win7 ≥65 → 60+数据、35~64 → **30+数据**、<35 → 整盘；剩余 <5GB 一律不建数据盘。新增 `PartInitPlan.createData`（diskpart 脚本/回读校验按需跳过数据盘）；对话框显示"将创建：系统盘 XGB + 数据盘约 YGB / 整盘"并随方案与输入实时更新；容量超可用空间明确报错。验证：`make check`（57/453 + i18n **543** + 门禁全过）+ `make package`（**`0.7.76`**）。🚧 仍需 VM 复测：60GB 盘 Win10 应为"整盘 53GB、无数据盘"；Win7 应为"30GB + 数据盘"；窗口按钮不再被裁。
  · **VM 实测 MBR 分区失败（2026-10-21，`0.7.77`）**：BIOS/Win10 VM 里分区后报"写入 MBR 引导代码失败（bootsect 不可用）"。根因：**`bootsect` 命令漏了 `/nt60`**——`bootsect X: /mbr` 被当成用法错误（返回非 0），旧代码把所有非 0 都误报成"不可用"。修：`bootsect /nt60 <盘符>: /mbr`；并区分两种失败（**PE 缺 bootsect.exe** → 提示换 GPT/手动命令；**返回错误** → 提示看日志/手动命令），日志记录完整命令与输出。i18n **544**；`make check`/`package`（**`0.7.77`**）。⚠️ 已失败的测试盘：在 PE 里手动 `bootsect /nt60 <系统盘盘符>: /mbr`，或 `diskpart` clean 后用 0.7.77 重来。
  · **VM 端到端（2026-10-21，`0.8.0`）**：修后同一台 BIOS/Win10 VM 里 **MBR 分区 → 还原 → 重启成功**。首次启动在"检测硬件"阶段蓝屏并自动重启一次，再启动即进桌面（判定为 Windows 首次驱动适配/重启周期，**暂不追**；同一镜像在 UEFI VM 直接正常）。该阶段系统尚无可写设备，救援日志未保留（后续可考虑在首启早期落盘，暂记）。

### PIT-160

- PIT-160 **异机还原（0.9，2026-10-21）：仅 Win10/11；最小化=启用 in-box 启动存储驱动；可选用户驱动包（DISM 离线注入）**：
  · **范围**：仅 Windows 10/11（`IsUniversalOsSupported` = major ≥ 10）；Win7/8.1 在暂存前拒绝并提示（同机还原 / PE 就地 / 新装）。异机只走 Windows 侧路径：**A 引擎（~new 解压后、交换前）** 或 **PE 就地还原（apply 后）**；B 引擎（Linux apply）与「安装菜单」直接拒绝（没有 DISM）。
  · **最小化（零体积）**：`EnableInboxStorageDrivers` 离线 `reg load` 目标 `Windows\System32\config\SYSTEM`，读 `Select\Current`，把 `storahci/stornvme/pciide/atapi/amdsata/amdxata/usbstor` 的 `Start` 设为 0（只改存在的服务）。
  · **可选驱动包**：用户选文件夹（不随包内置）；`ValidateDriverFolder` 递归数 `.inf`；`dism /English /Image:<root> /Add-Driver /Driver:<dir> /Recurse /LogPath:<logs>\dism-universal.log`。x64 镜像要求 64 位宿主/PE（`IsHost64`），否则拒绝。
  · **失败语义**：A 引擎注入失败 → 删除 `~new`、旧系统原样保留（可回退）；就地还原失败 → 明确报错（目标已写入，需用正确驱动包重试/手动修）；`reg load` 失败也按失败处理（提示）。
  · **界面/CLI（0.9.1~0.9.3 简化）**：还原模式只放一个 **「异机」勾选框**；勾选后**自动**使用程序目录 `Drivers\` 下的驱动（有 `.inf` 就注入，没有就仅启用内置驱动），不再弹设置窗。若用户选「重启后还原」立即提示不支持。CLI：`--universal [--drivers <目录>]`（显式目录优先）。**0.9.3/0.9.4**：`静默模式` → `静默`；两勾选框靠左、与右侧按钮留 22px 间隔（静默 248..330 / 异机 336..398 / 修复 420..490 / 日志 495..545 / 清除 550..620 / 菜单 625..709）；随包 `Drivers\README.txt` 改为**公开来源指引**：Dell WinPE 11 包（32MB，含 Intel RST/AMD RAID/LSI MegaRAID）、HP/Lenovo WinPE 包、virtio-win（BSD-3，可再分发）、SDIO、Microsoft Update Catalog，另加**社区整理包**：OSDeploy WinPEDriverPack 镜像（amd64 子目录，Intel-RAID 1.75MB/AMD 0.14MB/Dell 97MB/HP 126MB，按需取）、OSDeploy OSD 模块（自动抓厂商包）、52250179/WinPEDrivers（Intel VMD v19/v20 极简包）；**OEM 包只给链接、不再分发**。
  · **预置驱动（2026-10-21，本机 dist）**：`dist\Drivers` 已放入 **Dell WinPE11 包**（58 个 .inf，112.7MB，含 Intel RST/VMD、AMD RAID、LSI MegaSAS、网卡）+ **Intel VMD v19/v20**（各 1 个 .inf，共 7.2MB）+ **VirtIO**（virtio-win 0.1.271 的 viostor/vioscsi/NetKVM，Win10/11 x64，6 个 .inf / 3.1MB），合计 **66 个 .inf / 123MB**；来源与 SHA256 写在 `dist\Drivers\PRELOADED.txt`。**Fedora 官方下载有反爬**，VirtIO 改用社区精简镜像（`caoquocdung/virtio-win-iso-lite`，内容=官方 ISO 0.1.271）；⚠️ **GitHub 上 `viostor`/`vioscsi` 是源码，不能直接注入**，必须用 ISO/MSI 里的已签名成品。`dist` 在 `.gitignore` 内（驱动不入库）；dist 总大小约 **165MB**（删除 `Drivers` 即回到约 40MB）。**Dell 包仅本机/本 U 盘使用，不再分发**。
  · **体积**：代码增量 <0.1MB；**不内置厂商驱动包**（精选包由用户选择/自备），dist 维持约 40MB。
  · **验证**：`make check`（57 用例/453 断言 + i18n **582** + docs/widths/shell 全过）+ `make package` 双架构 + initramfs 重建（**0.9.0**）。🚧 **待实测**：① P0 矩阵 AHCI→RAID/VMD、SATA↔NVMe、物理↔VM（对照：不勾选应复现 0x7B）；② GUI 设置框与按钮状态；③ 就地还原路径注入；④ Win7 镜像拒绝提示；⑤ 驱动包位数/签名错误提示。
  · **教训**：① DISM 只能服务同/更高版本、同架构的离线镜像——x64 镜像别用 32 位 PE；② 驱动包只解决“启动关键”，不能保证芯片组/ACPI/激活；③ 新增含 Tr 的 `universal.cpp`/`universal_dlg.cpp` 必须加入 `WRAP_FILES`（本轮先漏、check C2 抓到）；④ 链接 `GetFileVersionInfoW` 需 `-lversion`。

### PIT-161

- PIT-161 **镜像路径输入汉字跑到最前面（UNC 路径）：聚焦时同步文本 + SetText 重置光标到 0；顺带目录被当成镜像**（2026-10-21 VM 实测，`0.9.5`）：
  · **现象**：在还原模式手动输入 `\\192.168.110.236\d\我的备份`，输到 `\d\` 后再打汉字，结果显示为 `份备的我\\192.168.110.236\d\`（字符逐个插到最前面）。
  · **根因**：① `CSkinEditUI::DoEvent` **每次事件**都 `SyncTextFromNative()`，而它调 `SetText` → `CEditUI::SetText` → `Edit_SetText` 会把原生 EDIT 光标重置到 **0**；② UNC 共享根 `\\server\share\` 在输入过程中是**合法目录**，`ImagePathReady` 仅看 `GetFileAttributes` → 被当成镜像去 `LoadWimImages`（同步阻塞），进一步触发事件/同步。二者叠加导致光标回 0，汉字逐个插到开头。
  · **修复**：① `DoEvent` **只在失焦/未聚焦时**同步（自绘需要时才同步）；② 重写 `CSkinEditUI::SetText`，程序化更新后把光标放到**末尾**；③ `ImagePathReady` 对 `FILE_ATTRIBUTE_DIRECTORY` 返回 false（目录不是镜像）。
  · **验证**：`make check`（57 用例/453 断言 + i18n 569/569 + 门禁）+ `make package`（**`0.9.5`**）。🚧 待 VM 复测：连续输入 UNC + 中文，顺序应为 `\\192...\d\我的备份`；目录/共享根不再触发阻塞读取。
  · **教训**：① 文本同步只能在“非聚焦”时做（聚焦时原生控件才是真身）；② 任何程序化 `SetText` 之后都要恢复/设置光标（否则接着输入会插错位置）；③ “路径存在”不等于“镜像文件”，目录要排除。

### PIT-162

- PIT-162 **网络共享（Unicode 机器名 `\\REDACTED-SERVER\...`）备份 rc=47：改用 CreateFileW + 写 fd，绕开 wimlib 的 `_wopen`；并加目标预检与 wimlib 错误日志**（2026-10-21，`0.9.6`）：
  · **现象**：客户 0.8.0 备份到 `\\<server>\<share>\<folder>\...wim` 报 `备份失败(rc=47): Failed to open a file`；同一共享其它软件正常。我们自己的中文**共享名**（`\\127.0.0.1\测试$`、`\\localhost\...`、本机名）备份全成功 → 不是通用中文/UNC 问题。
  · **分析**：目标机器名是 **Unicode（`REDACTED-SERVER`）**；wimlib 路径接口用 `topen = _wopen`（CRT）打开输出，某些环境下 CRT 对 Unicode 主机名的 UNC 转换失败 → rc=47。其它软件用 `CreateFileW`，故正常。另两种可能（需日志排除）：UAC 提权后账号与共享授权账号不同（拒绝访问）；共享只读/网络不可达。
  · **修复**：① `WimEngine::Capture` 改为 **`CreateFileW` 打开 `<dest>.tmp` + `_open_osfhandle` + `wimlib_write_to_fd`**（输出不再经 CRT 路径；简体/中文/Unicode 主机名与资源管理器同 API）；② `RunBackup` 新增**目标可写预检**（在目标目录建 `zjprobe-<pid>.tmp`，失败时把 Win32 错误码 5/53/67/64/3/2/123/206 翻译成可操作提示，含“UAC 提权账号可能不同”的提示）；③ `WimEngine` 构造时 `wimlib_set_error_file_by_name(<logs>\wimlib-errors.log)`，GUI 无控制台也能拿到 wimlib 的**具体失败路径/errno**。
  · **验证**：本机中文共享名（127.0.0.1/localhost/本机名）备份成功（fd 路径回归）；只读共享预检输出“错误码 5 + 提权账号提示”；`make check`（57/453 + i18n **575** + 门禁）+ `make package`（**`0.9.6`**）。🚧 待客户用 0.9.6 复测 `\\REDACTED-SERVER\...`；若仍失败，直接看 `logs\wimlib-errors.log` 与预检错误码。**已知边界**：`Append`（追加镜像）仍走 `wimlib_open_wim`（路径/CRT）→ 对 Unicode 主机名的追加可能仍受限（本轮先保证新建备份）。
  · **教训**：① 网络/Unicode 路径不要只靠 CRT 的 `_wopen`；关键输出用 `CreateFileW` 自己开；② 错误码要在**动作之前**预检并翻译（rc=47 的信息量几乎为零）；③ GUI 必须把库的详细错误落文件，否则现场只能看到“Failed to open a file”。
  · ⚠️ **2026-10-23 更正**：客户 0.9.6 的新日志（`wimlib-errors.log`）显示 rc=47 的真正失败文件是 **VSS 快照内的 OneDrive 应用缓存**（`...\AppData\Local\Microsoft\OneDrive\...\fre_done.svg`，`status=c0000034`），**不是** Unicode 主机名；目标预检已通过。修复见 **PIT-164**（默认排除 OneDrive 应用缓存）。`CreateFileW` 改动保留（对 Unicode 目标仍更稳），但不要把它当成本次根因。

### PIT-163

- PIT-163 **0.9.7：备份保存路径与还原镜像路径分离 + 任务执行中锁定 UI + 救援层减少误判英文；UEFI repair-boot 虚拟机实测通过**（2026-10-23）：
  · **备份保存位置被还原镜像路径顶回**（用户实测）：在“文件→系统”搜索到 `D:\2024...esd` 后切到“系统→文件”，手输 `D:\abc.esd`，点别处又变回搜索到的路径。根因：两个模式共用一个 `m_wimPath`，且路径同步依赖 `WM_COMMAND/EN_CHANGE`（原生 `CEditWnd` 的文本变化主要走 `DUI_MSGTYPE_TEXTCHANGED`），失焦/定时器可能把旧值写回。修复：新增独立 `m_backupPath`（`ActivePath()` 按模式取）；`BrowseSaveFile`/`StartBackup`/`SyncFormatFromPath`/`SyncPathExtFromFormat`/拖放/`UpdateMainAction` 全部走备份路径；`Notify` 增加 `DUI_MSGTYPE_TEXTCHANGED` → `SyncImagePathFromUi()`；模式切换时 `SetImagePathEdit(ActivePath())`。日志新增 `GUI mode: backup|restore path=...`。
  · **任务执行中锁定 UI**（用户规格：仅「静默」「限速」可点）：`SetBusyControls(m_busy)` 由 `UpdateMainAction()` 统一调用，禁用浏览/搜索/路径/子镜像/备注/分区/格式/模式 Tab/修复/日志/菜单/ESP/异机/主按钮/SiteLink；`ImagePath` 的原生 EDIT 同时 `EnableWindow(FALSE)`；`Silent` 与 `CpuCapBox` 保持可用；关闭按钮仍在 `m_busy` 之前处理（PIT-071 的取消流程不变）。`Notify` 的 busy 守卫只放行 `Silent` 点击、`CpuCapBox` ITEMSELECT 与最小化/最大化。
  · **救援层减少误判英文**：`init` 对 `blkid` 为空的分区（MSR/未格式化）打 `skip <dev> (no filesystem)`，不再把整段 `NTFS signature is missing` 刷屏；UEFI 下缺 bootfix 改为 `no bootfix needed (UEFI boot)`（BIOS 才 WARN）；`ufshcd-core/ufshcd-pci` 未打包时改为 `optional (not bundled; no UFS hardware)`，不再 WARN。`zjrestore-lite.sh` 的 image-scan 同款处理。
  · **日志增强**：`GUI repair-boot start/done rc=...`、`GUI menu install/remove request: image/disk/part/index/keepOld`、`GUI mode: ...`；UEFI 启动项日志带 `desc=SysRecover` 与 OptionalData 长度。下次客户再报“菜单/修复”问题，日志可直接区分入口与阶段。
  · **repair-boot 虚拟机实测（UEFI）**：把 `<lab>\e2e2\utarget.vhd` 复制成测试 VHD → 删除 ESP 的 `\EFI\Microsoft\Boot\BCD`（保留 `.bak`）→ 0.9.6/0.9.7 `SysRecover.exe repair-boot --disk 2 --part 4` → `bcdboot (UEFI) rc=0` + `displayorder {default}` + 校验通过；ESP 重新生成 BCD 20480B；QEMU + `edk2-x86_64-code.fd` 启动该 VHD，成功到 Windows 登录界面（截图 `<lab>\repairtest\shot3.png`）。**结论：UEFI repair-boot 路径可用**；BIOS 路径本轮未重复测（同一 `RepairBoot`，BIOS 分支此前 PIT-130 已测）。
  · **验证**：`make check` 57 用例/453 断言 + i18n 575/575 + check-docs/widths/shell PASS；`make package` 双架构（**`0.9.7`**，initramfs stamp 0.9.7，494 模块）。🚧 待用户实测：① 备份模式手输保存位置不再被还原路径覆盖；② 任务中除静默/限速外全部点不动、关闭仍弹「继续等待/终止并退出」；③ 客户再次走菜单/修复时日志更完整。

---

### PIT-164

- PIT-164 **0.9.8：rc=47 真根因 = VSS 快照内 OneDrive 应用缓存（status=c0000034）；补强备份路径失焦持久化 + 备份模式隐藏「安装菜单」+ 菜单禁用原因日志**（2026-10-23，客户 20261022 测试日志）：
  · **rc=47 更正**：客户 0.9.6 日志的 `wimlib-errors.log` 原文：
    `"GLOBALROOT\Device\HarddiskVolumeShadowCopy6\Users\Administrator\AppData\Local\Microsoft\OneDrive\26.163.0823.0004\images\darkTheme\fre_done.svg": Can't open file (status=c0000034)`。
    目标预检已通过、`dest=\\REDACTED-SERVER\...` 未报错 → **失败在源文件**，不是 Unicode 主机名/网络共享。`status=c0000034` = 对象名不存在（OneDrive 云占位/缓存文件在 VSS 快照中不可读）。修复：`DefaultExclusionConfig()` 增加 `\Users\*\AppData\Local\Microsoft\OneDrive*`（应用缓存，不是用户 OneDrive 数据目录；用户目录仍由 `CloudFolderNames` 按实际存在排除）。
  · **验证（可复现）**：临时目录造 `Users\X\AppData\Local\Microsoft\OneDrive\26.1\images\skip.txt` + `Users\X\Desktop\keep.txt` → `backup --source <临时目录> --no-snapshot` → `extract`：`keep.txt` 存在（rc=0），OneDrive 缓存路径 `The path does not exist in the WIM image`（rc=1）→ 排除生效。单测 `exclude_default_has_essentials` 增加该行断言（57 用例/454 断言）。
  · **备份路径失焦仍消失（0.9.7 首修不足）**：补 ① `CSkinEditUI::DoEvent` 在 `UIEVENT_KILLFOCUS` 先 `SyncTextFromNative()`，再 `SendNotify(DUI_MSGTYPE_TEXTCHANGED)`（把原生文本变化在失焦瞬间送回主窗口）；② `SyncImagePathFromUi()` / `EN_CHANGE` 在“原生 EDIT 已销毁且文本为空”时**不覆盖** `ActivePath()`，并从 `ActivePath()` 回填控件，避免 500ms 定时器把已输入路径清空。
  · **「安装菜单」灰色**：`ApplyModeUi()` 备份模式 `BootMenuBtn` 改为隐藏（该按钮是还原专属；此前备份模式也显示一个永远灰的「安装菜单」，用户误判）；`RefreshBootMenuBtn()` 在未安装且不可点时落日志 `GUI boot menu disabled: backup=… pathEmpty=… imageOk=… part=… busy=…`，下次可直接判断是路径没进 `m_wimPath`、镜像未解析、分区未选还是 busy。
  · **验证**：`make check` 57 用例/454 断言 + i18n 575/575 + check-docs/widths/shell PASS；`make package` 双架构（**`0.9.8`**，initramfs stamp 0.9.8，494 模块）。🚧 待客户用 0.9.8 复测 `\\REDACTED-SERVER\...` 备份；GUI 路径/锁定仍未自动化按键验证（Duilib 原生 `EditWnd` 不接受脚本 SetText/SendKeys），需用户实测。

### PIT-165

- PIT-165 **0.9.9：还原模式按钮改版（清除菜单/安装菜单同款样式 + 两两换位）**（2026-10-23，用户规格）：
  · **改名**：`RepairBootBtn` 的 `清除引导` → **`清除菜单`**（`skin/main.xml` + `tools/i18n-en.py` 的 `清除菜单=Clear Menu`；旧 `清除引导` 键已删）。`BootMenuBtn` 仍叫 **`安装菜单`**（用户后续明确：不是「安装引导」）。
  · **同款样式**：`RefreshBootMenuBtn()` 不再按 `can` 把 `BootMenuBtn` 的 `bkcolor` 设成 `C_DISABLED`；安装/未安装都保持 `#FFFFFFFF` 背景 + `C_TEXT` 文字（与 `清除菜单` 相同）。`SetEnabled(can)` 仍保留（不可用时只变文字色、不误点）。两个按钮宽度统一为 **84px**（`539..623` / `625..709`）。
  · **换位**：还原模式最终顺序 = `日志`(420..465) → `修复引导`(467..537) → `安装菜单`(539..623) → `清除菜单`(625..709)。即「修复引导/日志」互换、「安装菜单/清除菜单」互换；`skin/main.xml` 初始位置与 `ApplyModeUi()` 的 `Pos()` 同步。备份模式仍隐藏这四个按钮。
  · **门禁**：`check-widths` 首次因 70px 放不下 `Add Menu`(72)/`Clear Menu`(78) 报 FAIL；把两个按钮加宽到 84、`日志` 收到 45px 后通过。`make check` 57 用例/454 断言 + i18n 575/575 + check-docs/widths/shell PASS；`make package` 双架构（**`0.9.9`**，initramfs stamp 0.9.9，494 模块）。
  · 🚧 待用户看界面：按钮文字/顺序/宽度/禁用色是否满意；`清除菜单` 的英文是 `Clear Menu`。

### PIT-166

- PIT-166 **0.9.10 质量检查：Cppcheck/ShellCheck 静态分析 + 真实缺陷修复**（2026-10-23）：
  · **工具**：Cppcheck 2.22.0（`--enable=warning,performance --inconclusive --platform=win64`，仅 `src/`）与 ShellCheck 0.11.0（6 个救援脚本）。
  · **结果**：Cppcheck **0 error**、18 warning（15 处 C 风格 cast；2 处 VSS 误报；1 处 lambda `va_start` 误报）、10 performance；ShellCheck **0 error**、7 warning（`ZJPREFIX` 未 `-x` 跟随 `/zjlog.sh`、`ls`/printf 建议）。
  · **修复**：① `grub.cpp` `snprintf` 的 `%lc` → `%c` + `(char)`；② `zjrestore-lite.sh` EA `ADD="$ADD[...]"` → `${ADD}[...]`（SC1087）；③ hidden sectors / boot region 的 `A && B || C` → `if/else`（SC2015）；④ `target sectors` 先落变量、`echo "$_g2"` 加引号（SC2046/SC2086）；⑤ `zjswap.sh` `rm -rf "${M:?}/$b"`（SC2115）；⑥ `EfiHdNode`/`DeployItem`/`LogFileItem`/`AtaSmartIn`/`Hit` 成员默认初始化；⑦ 去掉 `uefi.cpp` 的 `(void)0;`。
  · **回归**：`make check` 57 用例/454 断言 + i18n 575/575 + check-docs/widths/shell PASS；QEMU BIOS drill **PASS**（重建 `base/drill.raw` 后）；`make package` 双架构 **0.9.10**。
  · **剩余（不阻塞 1.0）**：15 处 C 风格 cast 逐步改 `static_cast/reinterpret_cast`；`ops.cpp` 2725 行等大文件拆分；CI 仍只 x64 `check+all`，x86/package/QEMU 冒烟/静态分析未入门禁。报告见 [`docs/27`](docs/27-质量检查报告（0.9.10）.md)。

### PIT-167

- PIT-167 **0.9.11：客户 0.9.9 仍 rc=47，真凶换成 `ProgramData\Alibaba\AlibabaProtectDT\Logs` 的损坏日志（status=c0000102）→ 默认排除**（2026-10-23，客户 `E:\我的下载\logs`）：
  · **证据**：`wimlib-errors.log` 原文：
    `"GLOBALROOT\Device\HarddiskVolumeShadowCopy6\ProgramData\Alibaba\AlibabaProtectDT\Logs\2025-04-05-INFO-2180-1.log": Can't open file (status=c0000102)`（ShadowCopy7 同样）。
    `status=c0000102` = `STATUS_FILE_CORRUPT_ERROR`（文件/目录损坏，系统提示 Chkdsk）；目标 `dest=\\品管\...` 预检通过，**又是源文件问题**，不是 Unicode 主机名/OneDrive（0.9.9 已含 OneDrive 排除）。
  · **修复**：`DefaultExclusionConfig()` 增加 `\ProgramData\Alibaba\AlibabaProtectDT\Logs*`（第三方保护软件日志，不参与系统启动；避免损坏日志令整个备份 rc=47）。
  · **验证**：`make check` 57 用例/454 断言 + i18n 575/575 + check-docs/widths/shell PASS；`make package` 双架构（**`0.9.11`**，initramfs stamp 0.9.11）；单测新增该行断言。
  · **说明/后续**：这是第二类“源文件在 VSS 快照中不可读”（第一类 OneDrive 云占位，PIT-164）。**0.9.12 起不再按软件逐个补排除**：`WimEngine::Capture/Append` 注册 `WIMLIB_PROGRESS_MSG_HANDLE_ERROR`，对 `OPEN/OPENDIR/READ/READLINK/STAT` 设置 `will_ignore=true` 并写 `backup: skipped unreadable file: <path> (wimlib error=N)`；只对捕获/追加生效，**Apply/Verify 不忽略**（避免还原时静默丢文件）。客户仍可按具体路径补排除，但不再需要每次发版。

### PIT-168

- PIT-168 **0.9.12：不再按软件逐个排除——用 `WIMLIB_PROGRESS_MSG_HANDLE_ERROR` 通用跳过 VSS 快照里不可读的源文件**（2026-10-23，用户质问“难道要为全世界每个软件都升级一次”）：
  · **背景**：rc=47 先后由 OneDrive 应用缓存（PIT-164）、AlibabaProtectDT 损坏日志（PIT-167）触发；逐个加排除是打地鼠，任何软件的云占位/损坏/独占文件都可能再次触发。
  · **正解**：wimlib 的 `HANDLE_ERROR` 进度消息允许应用设置 `info->handle_error.will_ignore = true` 后继续。`ProgressThunk` 对 `WIMLIB_ERR_OPEN/OPENDIR/READ/READLINK/STAT` 设置忽略，并写日志 `backup: skipped unreadable file: <path> (wimlib error=N)`。
  · **边界**：`ProgCtx.skipFileErrors` 只在 `Capture`/`Append` 置 true；`Apply`/`Verify` 保持 false —— **还原/校验遇到写/读错误仍然 fail-closed**，不会静默丢文件。
  · **验证**：`make check` 57 用例/455 断言 + 门禁；`make package` 0.9.12；客户复测时即使还有其它不可读文件，也应继续备份并在 `SysRecover-*.log` 里列出跳过路径，而不是整盘失败。
  · **清单/确认（0.9.12 同批）**：`RunBackup` 把 Capture/Append 的 skipped 路径聚合成 vector（回调内互斥、去重）；GUI 备份完成时弹「备份不完整」清单（最多 20 条 + 完整日志路径，静默模式也弹，属 fail-safe 确认）；CLI 打印清单；日志收尾写 `backup: incomplete, skipped N`，history detail 记 `ok-skipped`。

### PIT-169

- PIT-169 **0.9.13：跳过文件必须让用户知道——备份完成弹「备份不完整」清单 + CLI 清单 + 日志汇总**（2026-10-23）：
  · **背景**：0.9.12 的通用跳过解决了“单个文件令整盘失败”，但用户不知道哪些内容没备份；备份工具不能静默丢数据。
  · **实现**：`RunBackup` 新增 `std::vector<std::wstring>* skipped` 出参；`WimEngine::Capture/Append` 的回调把跳过路径写入 `ProgCtx.skipped`（互斥、去重）；主镜像/ESP/EA 三处 Append 共用同一清单。GUI 备份成功且清单非空时弹「备份不完整」（最多 20 条 + 完整日志路径，静默模式也弹）；CLI 打印清单；日志写 `backup: incomplete, skipped N`；history detail `ok-skipped`。
  · **i18n**：新增 8 个词条（清单标题/更多/日志路径/状态），`check-i18n` **584/584**。
  · **验证**：`make check` 57 用例/455 断言 + 门禁；`make package` **0.9.13**；调研与结论见 [`docs/28`](docs/28-备份打开文件处理调研.md)。
  · **边界**：`HANDLE_ERROR` 主要覆盖扫描阶段；数据读取阶段错误仍可能失败；跳过=备份不完整，UI/CLI 只告知，不自动修复源文件。

### PIT-170

- PIT-170 **日常还原临时菜单与「安装菜单」分离：不同名称/GUID + 首启“恢复引导菜单”清理器**（2026-10-23，`0.9.21`）：
  · **规格（用户）**：只有用户显式「安装菜单」才保留常驻启动菜单；日常还原不应留下菜单，二者必须使用不同名字。
  · **实现**：
    - UEFI：常驻 `SysRecover`（`Boot####`）vs 临时 `SysRecover Temporary Restore`；BIOS：常驻 `RecoveryGuid`/`SysRecover Recovery Environment` vs 临时 `TemporaryGuid`/`SysRecover Temporary Restore`（`src/boot/bootnames.h`、`bcd.h`）。
    - 日常暂存/回旧系统走临时条目；`InstallUefiBootEntry`/`InstallBootLayer`/`SetUefiBootNext` 增加 `temporary` 参数；`RemoveBootLayer` 两者都清，首启清理只清临时项。
    - 显式安装菜单写 `restore-menu.flag`；检测到常驻菜单时普通还原/回旧系统**拒绝**并提示先「清除菜单」，避免覆盖用户菜单。
    - `src/tools/restore_boot_main.cpp` → `bootfiles/zj-restore-boot.exe`（x86、**不嵌提权清单**）：A/B 引擎都在公共启动文件夹放 `SysRecover-RestoreBoot.cmd`；首次登录先以普通权限启动 helper，helper **先弹说明框**，用户确认后才 `ShellExecute(runas)` 请求 UAC；UAC 取消则下次登录重试，成功后写 `boot-cleanup.done` 并自删 cmd。
    - helper 删除临时 UEFI/BCD 条目；无常驻菜单时再清 ESP `\EFI\ZJRESTORE` 和系统盘根 boot 文件；回旧系统同样投放到 `~old`。
  · **B 引擎补充**：`--engine block` 没有离线 `~new`，由 Linux 救援层把 helper + 启动 cmd 投放到目标 `ProgramData`；A 引擎由 Windows 侧写入 `~new` 的两份文件（不再用 RunOnce）。
  · **边界/注意**：首启程序需要用户确认提示框 + 一次 UAC；Win7 目标需自备 x86 UCRT（helper 未随包放 UCRT 到 ProgramData）；B 引擎登录后清理，不是登录前。
  · **验证**：`make check` 58 用例/459 断言 + i18n **584/584** + check-docs/widths/shell PASS；`make package` 双架构 **0.9.21**（helper 373,248B、asInvoker）；QEMU B 引擎 drill 已断言 `boot cleanup: deployed`；🚧 待真机/VM 回归（提示框在 UAC 之前、UEFI 临时项首启消失、常驻菜单保留、BIOS/回旧系统）。

### PIT-171

- PIT-171 **CLI 参数穷举校验：未知选项/缺值/非法数字/非法压缩级别不再静默；顺手清理三处编译器警告**（2026-10-24，`0.9.27`）：
  · **风险**：旧 `Opt()` 对缺值返回默认、未知选项直接忽略；`atoi("abc")=0` 会让 `restore --disk abc` 静默针对磁盘 0；`Has("--yes")` 只看 token，拼写错误可能绕过确认。
  · **修复**：`ValidateCommandArgs()` 按命令白名单校验（backup/restore/verify/repair-boot/returnold/images/extract/diag/support/scan-ea/shortcut/list/version/history），未知选项/多余参数/缺值 → rc=2；`OptInt()` 用 `strtol` + 全串/范围校验，`--disk/--part/--index` 非法不再变 0；`--compress` 仅接受 `fast/maximum/recovery`。补齐既有别名 `extract --all/--image`、`history --last`、`shortcut --startmenu`；实现 `list --json`（机器可读 JSON），日志统一改走 stderr，保证 stdout 只含命令结果；隐藏的 `--apply` 直写路径已移除（绕过安全检查）。
  · **顺手修警告**：`partinit.cpp` 删除只写不读的 `protective`；`selfdiag.cpp` 注释行尾反斜杠；`disk.cpp` 的 `GetFirmwareType` 函数指针改 `memcpy`（消除 `-Wcast-function-type`）。
  · **验证**：`make check` 58/459 + i18n **586**；`smoke-cli` 新增未知选项/多余参数/缺值/坏 disk/坏 part/坏 compress/坏 repair 参数，rc 全 2；全量 `-B` x64 编译 **src 零 warning**；`dist-check` PASS；QEMU B/swap/undo/auto-recover 全 PASS。🚧 待推 CI。

### PIT-172

- PIT-172 **所有下拉框用上下键只能选到最下方一项：Duilib `CComboUI::DoEvent` 的快捷键 switch 漏 `break`，一路贯穿到 `VK_END`**（2026-10-25，`0.9.31`）：
  · **现象**：`FormatBox`、`CpuCapBox`、`ImageIndexBox`、`PartitionBox` 鼠标选择正常；打开下拉后按 ↑/↓ 总是变成最后一项，无法逐项遍历。快捷键打开后同理。
  · **根因**：`third_party/duilib-master/DuiLib/Control/UICombo.cpp::CComboUI::DoEvent` 的 `switch(event.chKey)` 每个 `case` 都**没有 `break`**：`VK_UP` 执行后会继续贯穿 `VK_DOWN`/`VK_PRIOR`/`VK_NEXT`/`VK_HOME`/`VK_END`，最终由 `VK_END` 选中 `GetCount()-1`；`VK_F4` 也会贯穿改选项。
  · **修复**：每个 `case` 后补 `break;`（`VK_F4` 打开后不再改选择），保留 `CComboWnd` 的 ESC/Enter 处理；`CPartitionComboUI` 继承同一基类，自动受益。这是第三方补丁，注释已写在源码中，升级 Duilib 时必须复核。
  · **验证**：`make check` / `make package` / `dist-check` 绿；GUI 人工/自动化 keyboard 冒烟待执行（鼠标路径不受影响）。

### PIT-173

- PIT-173 **用户 2026-10-26 四项实测：①异机还原把 Win10 判成 6.2 ②新分区后 ESP 找不到 ③部分镜像“缺少 ntoskrnl.exe” ④分区向导无边框**（`0.9.33`）：
  · **①/③ 版本探测（真根因）**：`ntoskrnl.exe` / `winload.exe` 的 `VS_FIXEDFILEINFO.dwFileVersionMS` 在 Win10 仍是兼容版本 **6.2**，真正的 `10.0.19041` 在 `StringFileInfo\...\FileVersion` 字符串里；旧 `ProbeImageOs` 读 fixed info → 所有 Win10 都被误判 6.2。**修复**：优先解析 `StringFileInfo` 的 `FileVersion`/`ProductVersion`（多语言翻译表），失败才回退 fixed info；同时取 `ntoskrnl` + `winload`，任一个可读即可判版本，单文件提取失败再分别补取（解决“镜像内缺少 ntoskrnl.exe”）。版本日志打印两边与最终选择。
  · **① 附带**：`PeMachineIs64` 原来把整个文件读入内存且设 8MB 上限，`ntoskrnl` 10.8MB → 永远判 x86；改为只读 DOS/NT 文件头，Win10 镜像现在正确识别 `10.0 x64`。
  · **② ESP**：新分区/刚格式化的 ESP 常无盘符，`FindFirstVolume` 的真实卷名与 `GUID` 可能暂时不同步；`PartitionInfo` 新增 `volumeName`，`AcquireEspRoot` 优先用它，并加 3 次重试（300/700ms）。`MountEspByGuid` 先走 Win32 `SetVolumeMountPointW`（精简 PE 可能没有 `mountvol.exe`），失败才回退 `mountvol`；`UnmountEsp` 同理用 `DeleteVolumeMountPointW`。失败时把 `blog` 写进主日志（此前只拼进用户提示）。
  · **④ 边框**：分区向导是纯 Win32 窗口，在部分 PE/主题下标题边框不可见；`WM_PAINT` 在客户区边缘画 1px 灰色 `FrameRect`。
  · **验证**：`make check` **58 用例 / 459 断言** + i18n **586/586** + docs/widths/shell/config/layers/secrets PASS；实际 Win10 ESD 日志 `ntoskrnl=10.0.19041 winload=10.0.19041 -> 10.0.19041 ... x64`；构造只含 `winload.exe` 的 WIM → `ntoskrnl=unknown winload=10.0.19041` 仍通过（不再报缺少 ntoskrnl）；GPT 新分区 + ESP 无盘符的就地还原实测成功；`make package` 双架构。🚧 待用户原 PE 复测 ESP 与分区向导外观。

### PIT-174

- PIT-174 **用户 2026-10-26 三项：①还原确认框矮 ②异机注入阶段 100% 后长时间无反馈 ③NewBee PE 无法启动**（`0.9.34`）：
  · **① 确认框**：`CConfirmDlg` 新增 `extraHeight`（仅还原确认传 40px；内容/按钮文案不变，按钮整体下移）。`Ask2` 就地还原、`Ask3` 需重启两条路径都加高；其他确认框保持原尺寸。
  · **② 异机注入进度**：`PrepareUniversal` 新增 `ProgressFn`，依次上报 `universal-start / universal-inbox / universal-inbox-done / universal-drivers / universal-done`；GUI 在主进度 100% 后显示独立小进度条 `PostProgress`（每 120ms 循环），状态栏显示“正在启用内置存储驱动 / 正在注入驱动包 / 驱动注入完成”，不再像死机。`ProgressUpdate("restore",…)` 同步写外部进度。
   · **③ NewBee PE 真因**：`C:\PE20210523NewBee.iso` 的 `boot.wim` 是 **Win10 19042 x64、没有 SysWOW64/WOW64**；发布包根目录 `SysRecoverUI.exe` 是 32 位启动器，在这种纯 x64 PE 里**加载阶段就失败**，根本走不到自举 `x64\SysRecoverUI.exe`。应直接运行 **`x64\SysRecoverUI.exe`**（CLI 用 `x64\SysRecover.exe`；PIT-175 已改为根目录 x64 总入口，删除 `Start-x64.cmd`）。另该 PE 没有 `dism.exe`（异机注入不可用）、`mountvol.exe`（已由 Win32 `SetVolumeMountPointW` 兜底）、`bcdedit.exe`（`VerifyEspBcd` 缺工具时改为文件级校验，不再误判）。已在 `resources/README.txt` 写明 PE x64 入口与 DISM 限制。
  · **验证**：`make check`（58/459 + i18n **592** + docs/widths/shell/config/layers/secrets PASS）；`make package` 双架构；确认框/小进度条外观与 NewBee 原 PE 待用户复测。

### PIT-175

- PIT-175 **发布入口重构：根目录 x64 带图标总入口 + `*_x86.exe` 兼容入口；删除 `Start-x64.cmd`**（2026-10-26，用户规格，`0.9.35`）：
  · **背景**：纯 x64 PE（NewBee 等无 WOW64）无法加载 32 位根启动器，之前的 `Start-x64.cmd` 仍让用户自己选，体验差。
  · **新布局**：根目录 `SysRecoverUI.exe` / `SysRecover.exe` = **x64 小启动器**（GUI 带品牌图标、提权清单），分别调用 `x64\SysRecoverUI.exe` / `x64\SysRecover.exe`；32 位 Windows 用同目录 `SysRecoverUI_x86.exe` / `SysRecover_x86.exe`。x64 真程序仍在 `x64\`，共享 `skin/lang/bootfiles/Drivers` 在根。
  · **实现**：`src/launcher/launcher.cpp` + 两个 `.rc`；`Makefile` 的 `ARCH=x86` 产物改为 `*_x86.exe`，新增 `launchers` 目标并挂到 `package`；删除 `resources/Start-x64.cmd` 及打包拷贝。
  · **验证**：根 `SysRecover.exe version` 成功调用 x64 CLI；根 `SysRecoverUI.exe` 启动后子进程为 `x64\SysRecoverUI.exe`；`make check` 58/459 + i18n 592；`make package` 双架构。🚧 待真机/PE 复测 x64 总入口与 x86 兼容入口。

### PIT-176

- PIT-176 **还原确认框按钮被裁：固定 +40px 只下移按钮、窗口高度没有同步；改为按文字行数自适应**（2026-10-26，`0.9.36`）：
  · **现象**：NewBee/10586 PE 的三行确认框，第三行文字与按钮间空出约三行，按钮只露上边（`1111.png`）。
  · **根因**：`m_extraHeight=40` 只加在按钮 `y` 上，`RunModal` 的窗口高度/`InitWindow` 没有同一套几何；窗口底部把按钮裁掉。
  · **修复**：移除固定 40px；统一用 `MessageBottom(lines) / ButtonTop(lines) / WindowHeight(lines)`：三行 196、四行 218、五行 240、六行 262；窗口高度永远 = 按钮底部 + 18px。所有确认框（Ask2/Ask3/Ask4）共用同一几何。
  · **验证**：`make check` / `make package`；三行确认框应有 18px 文字-按钮间距且按钮完整可见；🚧 待真实 PE 截图复测。

### PIT-177

- PIT-177 **PE 10586 新分区 GPT 仍报“找不到 ESP”：`PartitionInfo.letter` 为空，按卷标/大小找已有 ESP 盘符；support 补收程序目录 logs**（2026-10-26，`0.9.37`）：
  · **日志证据**：0.9.36 的 `drive-map` 显示 `p1 letter=- label=[ESP]`，但用户在 Explorer 里看到 ESP 有 E:；`list` 同样无盘符。说明 PE 的卷 ↔ 分区 extent 映射丢失/延迟，`FindEspPartition` 找到了 ESP，但 `esp.letter` 为空；`MountEspByGuid` 的卷 GUID 路径在该 PE 也没成功。support 包里没有主日志（只有 dism/history），因为 GUI/CLI 日志根可能不同。
  · **修复**：`AcquireEspRoot` 新增 ②c `FindEspDriveByLabel` —— 扫 D..Z 的已有盘符，只接受 **卷标 `ESP` + FAT/FAT32 + 总大小与目标 ESP 分区一致（±1MB）** 的卷，直接使用其根目录。它在 `MountEspByGuid` 失败后兜底，不会误选 NTFS 数据盘。
  · **support**：`BuildSupportBundle` 额外收 `<程序目录>\logs`（前缀 `app-logs`），覆盖 GUI 与 CLI 日志根不一致导致主日志缺失。
  · **验证**：`make check` / `make package`；🚧 待用户在 PE10586 用 0.9.37 复测新分区 GPT 的 ESP 修复；若仍失败，日志会多出 `ESP via label/size fallback` 或完整 `ESP acquire log`。

### PIT-178

- PIT-178 **停止猜 ESP：独立 `ESP-diag.txt` 诊断（不依赖主日志）+ 每个候选盘符/卷/挂载点全量记录**（2026-10-26，`0.9.38`）：
  · **背景**：0.9.37 的 label/size 兜底仍失败；support 包又缺主日志，无法确认 `FindEspPartition`/`letter`/卷标/大小/挂载失败点。用户明确要求先强化日志，不再猜。
  · **实现**：`ops.cpp` 新增 `EspDiagLine`/`EspDiagVolumes`，直接写 `<日志根>\logs\ESP-diag.txt` **和** `<程序目录>\logs\ESP-diag.txt`（不依赖 `LogInfo` 主日志，support 的 logs 树必收）。每次 `AcquireEspRoot` 记录：attempt、targetDisk、全部分区、`FindEspPartition` 的 matched/letter/volumeName/guid/offset/size、`mountvol /s` 结果、盘号校验、existing letter、volume GUID、label/size fallback、FAT/EFI fallback、最终失败。
  · **盘符清单**：`EspDiagVolumes` 记录 C..Z 的 `type/label/fs/total/volume GUID`，可直接看出 E: 到底是什么卷、是否 ESP/FAT、大小是否等于 p1；`FindEspDriveByLabel` 的候选筛选也走同一份数据。
  · **support**：`BuildSupportBundle` 继续额外收 `<程序目录>\logs`（PIT-177），因此即使主日志缺失，`ESP-diag.txt` 也会进包。
  · **验证**：`make check` / `make package`；🚧 待 PE10586 用 0.9.38 复现后导出日志，按 `ESP-diag.txt` 定因，不再改逻辑猜测。

### PIT-179

- PIT-179 **ESP-diag 定因：PE10586 的 E: 卷总大小比 p1 分区小 4MB，0.9.38 的 1MB 容差把正确 ESP 排除；放宽到 16MB**（2026-10-27，`0.9.39`）：
  · **日志证据**：`ESP-diag.txt` 显示 `E: type=3 label=ESP fs=FAT32 total=310378496 volume=`，p1 `offset=1048576 size=314572800`；差 **4,194,304 字节（4MB）**。`FindEspPartition matched=1`，但 `letter`/`volumeName` 为空；label/size fallback `found nothing` 正是因为 4MB > 1MB 容差。
  · **修复**：`FindEspDriveByLabel` 改为绝对差 ≤ **16MB**，仍要求卷标 `ESP` + FAT/FAT32；说明 FAT32/分区保留区会使 `GetDiskFreeSpaceEx` 的 total 与分区长度不完全相等。`E: volume=` 为空也不影响（直接用盘符根）。
  · **验证**：`make check` / `make package`；🚧 待 PE10586 用 0.9.39 复测：日志应出现 `ESP via label/size fallback at E:\`，随后 bcdboot 成功。

### PIT-180

- PIT-180 **用户六项界面改动：还原模式第三行重构（打包日志 / 安装到启动 / 「更多功能>>」下拉）+ 备份模式文案与限速顺序 + 分区向导进度**（2026-10-27，`0.9.40`）：
  · **① 还原模式第三行**（`skin/main.xml` + `main_form.cpp::ApplyModeUi`）：`日志` → **`打包日志`**（x420..465）；`修复引导` 收进「更多功能>>」；`安装菜单` → **`安装到启动`**（x539..623，**只安装**：已安装时置灰 + tooltip 指引到「更多功能>>」清除，删除入口唯一化）；新增 **`更多功能>>`**（x625..709，`SkinButton` + 原生 `TrackPopupMenu`，四项：`使用异机还原`[带 `MF_CHECKED`] / `修复引导` / `清除启动中还原菜单`[未安装时 `MF_GRAYED`] / `镜像检查（完整校验）`[镜像未解析时 `MF_GRAYED`]）。控件是顶层 `float` → `GetPos()` 已是客户区坐标，`ClientToScreen` 后弹出。
  · **② 异机勾选框移入菜单**：控件仍是隐藏的 `Universal`（`GlyphCheck` 派生 `COptionUI`），菜单项状态读 `IsSelected()`、切换调 **`Selected(on, false)`**；切换后状态栏提示 + `LogInfo("GUI universal: on|off")`。
  · **③ 镜像检查**：接 `WimEngine::Verify`（全量 `wimlib_verify_wim`，与 CLI `verify` 同源；0=成功），先弹确认框讲清"几十 GB 可能十几分钟"（默认取消），完成消息 **`WM_VERIFY_DONE = WM_APP+25`**，失败按 CLI `CmdVerify` 的 rc=5 处理。
  · **④ 备份模式**：`ESP` → **`同时备份ESP`**；CPU 限制顺序改为 **`75 / 50 / 25`**（`不限速` 不变）。
  · **⑤ 新硬盘分区向导进度**（`partinit.cpp/h`）：`ApplyPartInitPlan` 新增阶段回调（`letters` 5% / `script` 15% / `diskpart` 25→70% / `bootsect` 75% / `verify` 90% / `done` 100%），GUI 经既有 `WM_PROGRESS_UPDATE` 显示，`StageCn` 翻成中文；回调返回 `void`（不参与取消）。
  · **⚠️ 坑 1（编译门禁）**：`COptionUI` **没有 `SetSelected`**，状态切换接口是 `Selected(bool bSelected, bool bTriggerEvent=true)` —— `make check` 只链纯逻辑 TU、**不编译 GUI**，此错只有 `make package` 才抓得到（再次验证 PIT-153/154 的"最终判据必须是 `make package`"）。
  · **⚠️ 坑 2（菜单自动化验证）**：`TrackPopupMenu` 的菜单窗口（类名 `#32768`）**不吃 `PostMessage` 的鼠标按键**（不产生高亮 → 抬起只关闭菜单、不触发命令）；键盘 `WM_KEYDOWN` 同样无效。**可靠做法**：`GetWindowRect` 取菜单屏幕坐标 → `SetCursorPos` + `mouse_event(LEFTDOWN/LEFTUP)` 真实输入。实测两轮 `GUI universal: on` / `off`。
  · **⚠️ 坑 3（确认框参数顺序）**：`CConfirmDlg::Ask2(hwnd, title, message, leftText, rightText, defaultIsRight)` —— 新增调用点别把 title/message 写反（`StartVerifyImage` 即按此顺序）。
  · **验证**：`make check` **58 用例/459 断言** + `check-i18n` **612/612** + widths（1 既有 WARN）+ shell/docs/config/layers/secrets 全 PASS；`make package` 双架构 + `make sha256`（349 文件）+ `make dist-check` PASS + `make smoke-cli` 19 项 PASS；GUI 截图确认两模式第三行文案/顺序（还原：静默 / 打包日志 / 安装到启动 / 更多功能>>；备份：静默 / 同时备份ESP / 不限速），「更多功能>>」菜单弹出与切换实测通过。🚧 待实测：新硬盘分区向导进度条逐步走动（需空盘 VM）；`镜像检查` 对真实大镜像的进度与失败提示；`安装到启动` 已安装置灰与「清除启动中还原菜单」联动。

### PIT-181

- PIT-181 **用户 2026-10-27 四项需求：`更多功能M>>` / 独立「镜像检查」程序 / 分区向导分步提示 / 启动项写后读回校验（防安全软件静默拦截）**（`0.9.41`）：
  · **① 「更多功能>>M」→「更多功能M>>」**：`skin/main.xml` 的 `MoreBtn` 直接改文案并保留 `shortcut="M"`；关键是 `ui_skin.cpp::ShortcutHit` 原来在**词内命中**时会把快捷键字母追加到词尾（"更多功能>>" + M = "更多功能>>M"），新增 **`ShortcutPrefixW`**：词内命中只画下划线、不追加尾随字母（词尾/词首命中仍追加），两条路径共用同一词条文本。**纪律**：皮肤文案里已经写了字母的（如 `更多功能M>>`）不能再让渲染层追加一次。
  · **② 独立「镜像检查」程序 `SysRecoverCheck.exe`**（`src/tools/wimcheck_main.cpp`，console）：镜像路径参数（多个/拖入均支持）/ `--quick` 只快检 + 列子镜像 / `--help` / `--lang`；**无参且独占控制台**（`GetConsoleProcessList()==1`）时弹 `GetOpenFileNameW`（`OFN_ALLOWMULTISELECT`，64KB 缓冲）→ 双击可用、拖入可用、脚本可用（管道场景无参直接给用法 rc=2，不会卡住等对话框）。检查链 = `WimEngine::Probe` 快检（PIT-057 半截镜像防护）+ `ListImages` 列子镜像 + `Verify` 全量带进度；退出码 **0/1/2/5/6**（Ctrl+C → 6），末尾独占控制台才 `暂停`。**实现纪律**：输出统一走窄串 UTF-8（`T()` = `Tr` + `ToUtf8`，`printf("%s")`），彻底避开 PIT-007 的 `wprintf %s` 坑；`SetConsoleOutputCP(CP_UTF8)`；**不嵌提权清单、不带图标、不装崩溃处理器、不调 `ReexecX64IfNeeded`**（纯只读工具，普通权限即可跑）。产物三处：`dist/x64/SysRecoverCheck.exe`（`CHECK_NAME`）、根 `dist/SysRecoverCheck.exe`（`launcher.cpp` 的 `ZJ_LAUNCH_CHECK` 分支：console + wait=true，**不带 `.rc`**）、`dist/SysRecoverCheck_x86.exe`。Makefile 目标名必须叫 **`wimcheck`**（`check` 已被门禁占用），链接补 **`-lcomdlg32`**（`GetOpenFileNameW`）。
  · **③ 分区向导分步提示**：原实现把整段 diskpart 脚本一次丢给 `diskpart`（几十秒无任何反馈）。改为 `src/disk/partscript.{h,cpp}` 纯逻辑生成器 —— `BuildDiskpartSteps` 按「清表 / 转换 / ESP / MSR / 系统盘 / 数据盘」拆成**多次独立 diskpart 调用**，`JoinDiskpartSteps` 供单测拼回校验；`partinit.cpp` 改成循环 `WriteAsciiFile` → `RunProcess` → rc 检查，**每步之前** `report(pct, stage)`（12..82 均分），再 bootsect 86 / verify 90 / done 100；日志 `partinit: step <stage> rc=N`。GUI 经既有 `WM_PROGRESS_UPDATE` 显示（`StageCn` 补 6 个新阶段中文）。
  · **④ 启动项写后读回校验（客户实测：装了金山毒霸/360 的机器上两种还原路径都在重启后进原系统、看不到启动选单）**：根因是 `bcdedit` 的**假成功**（返回 0 但值没落盘），旧代码完全信任退出码。现在所有启动项写入都**写后读回**：
    - `bcd.h` 新增纯逻辑 **`ParseBootsequence`**（只认 `bootsequence` 这个 **ASCII 元素名**、大小写不敏感、值必须是 38 字符 `{...}`，与 `ParseBootmgrTimeout` 的"不依赖标签语言"同款纪律）与 **`SameGuidAscii`**（GUID 比较大小写不敏感）。
    - `BcdSetBootsequence(guid, log)`：**两轮**「写 → 读回 → 比对」，失败日志 `bootsequence verify FAILED (wanted …, got …)`；`BcdSetBootmgrTimeout` 同样两轮读回；`BcdCreateBootsector` 末尾用 `BcdEntryExists` 复核（`entry verify ok` / `entry verify FAILED`）。
    - `uefi.cpp::SetUefiBootNext`：两轮「`SetVarW` → `GetFirmwareEnvironmentVariableW(L"BootNext")` 比对」，日志 `BootNext = Boot%04X (verified)` / `verify FAILED`（固件变量同样会被安全软件拦）。bootapp 分支改调 `BcdSetBootsequence`。
    - `ops.cpp` 新增 **`BootWriteBlockedHint()`**（返回 `std::string`：`Tr()` 窄重载返回 `const char*` UTF-8，`err` 是 `std::string`），三处调用点（安装菜单 timeout / 暂存单次启动 / 返回旧系统单次启动）失败时**追加该提示并中止**（不再"写失败也照样重启进原系统"）；成功时 `LogInfo("bootsequence verified: " + slog)`。
  · **实测（本机真实 BCD，写→读→还原，零残留）**：`bcdedit /enum {bootmgr}` 回显形态 = `bootsequence            {GUID}`（38 字符 ✓）；**GUID 回显被 bcdedit 转成小写**（我们写 `…CBA987654321`，回显 `…cba987654321`）→ 正好验证 `SameGuidAscii` 大小写不敏感的必要性；另实测 **`{current}` 别名会原样回显**（不展开成 GUID）—— 我们只写显式 GUID，不受影响，但任何"写别名再比对 GUID"的新写法都会踩这个坑。
  · **门禁**：`make check` **61 用例/532 断言**（新增 `bcd_parse_bootsequence` 11 断言 + `same_guid_ascii` 7 断言 + `part_script_steps`）+ `check-i18n` **650/650**（新增 3 键：安全软件提示、单次启动失败、检查程序串）+ docs/宽度/shell PASS；`make package` 双架构 + `make sha256`（**352** 文件）+ `dist-check` PASS；**`smoke-cli` 扩到 24 项**（新增第二个参数 = 检查程序：`--help` / 未知选项 / 无参 / 不存在文件 / `--lang en help`）。实测：`--quick` rc=0（列 1 个子镜像）、全量校验 rc=0（进度行「校验镜像」中文生效、673 MB/s）、不存在文件 rc=1、`--badopt` rc=2、根启动器透传 rc、`SysRecoverCheck_x86.exe --lang en` 英文正确；输出落盘用 Python 复核 = **合法 UTF-8、零替换字符**（PowerShell 5.1 控制台里 `。`/`：` 显示成 `?` 是 PS 解码假象，**不是程序问题**）。
  · **⚠️ 坑（本轮）**：`src/disk/partscript.cpp` 只加进了 `TEST_UNITS` 没进 `APP_SRC` → `make check` 全绿（单测自己链了）而 `make package` 链接报 `undefined reference to BuildDiskpartSteps/JoinDiskpartSteps`。**再次确认：`make check` 不编译 `ops.o`/GUI/`partinit.cpp`/`src/launcher`/`src/tools`，最终判据必须是 `make package`**（PIT-153/154/180 同款）。另：`tools/check-dist.py` 的 `REQUIRED` 已补三个 check exe；`tools/i18n-wrap.py` 的 `WRAP_FILES` 已加 `wimcheck_main.cpp`（PIT-109 教训：漏加会静默删词条）。
  · 🚧 **待实测**：① 客户那台装流氓软件的机器（需求 4 的原始场景）——应看到「可能被安全软件…」提示并中止，而不是重启后进原系统；② `SysRecoverCheck.exe` 双击（弹多选文件框）与拖入镜像；③ 空盘 VM 里分区向导逐步进度 + 小容量磁盘规则。

### PIT-182

- PIT-182 **双击根目录 `SysRecoverCheck.exe` 不弹文件框、窗口一闪即关：启动器与子进程**共用**一个控制台 → `GetConsoleProcessList()`=2 → 子进程把自己当成"脚本调用"**（2026-10-27，`0.9.42`）：
  · **现象**：直接跑 `dist\x64\SysRecoverCheck.exe`（双击/自建控制台）**正常**：新控制台 + 弹「选择要检查的系统镜像」`#32770`；但双击**根目录**的 `dist\SysRecoverCheck.exe`（x64 启动器）只看到控制台闪一下就没了 —— 既不弹框、也看不到任何输出。
  · **根因**：`wimcheck_main.cpp::OwnConsole()` 用 `GetConsoleProcessList()==1` 判"这个控制台是我自己的"（决定**弹文件框**与**结束时等回车**）。根启动器是 `-mconsole` 程序，双击时**它先拿到**新控制台，再 `CreateProcessW` 拉起 `x64\SysRecoverCheck.exe`（**继承**同一控制台，这是 `CreateProcessW` 的默认语义）→ 进程数 = **2** → 子进程判"不是我的控制台" → 当成被脚本调用：无参时直接打用法 rc=2、跑完立即退出，控制台跟着启动器一起消失。**判据本身没错**（它要挡的正是"`make smoke-cli`/管道调用被暂停卡住"），错在启动器这种"其实是我自己的控制台、只是多了一个中间人"的情形。
  · **修法（一句话交接，不猜）**：启动器在 `CreateProcessW` **之前**调 `MarkConsoleForChildIfOwned()` —— 仅当 ① 控制台进程数 == 1（确实是自己独占的）**且** ② 父进程不是自动化宿主 时，`SetEnvironmentVariableW("ZJ_CHECK_LAUNCHER","1")`（**环境变量随 CreateProcessW 自动继承**）；子进程 `OwnConsole()` 先认这个标记 → 照常弹框/等待。
  · **为什么要判父进程（否则门禁会挂）**：本会话的 PowerShell / 计划任务 / `make smoke-cli` 都是"无控制台或独占控制台"的宿主 —— 若只判进程数，启动器会给子进程打标记 → `smoke-cli` 的 5 个检查程序用例**每次多等一次回车**（无人值守会一直挂着）。故新增 `StartedByAutomationHost()`：`CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS)` 找自己的 `th32ParentProcessID`，再按名字 `_wcsicmp` 比对名单 `cmd.exe / powershell.exe / pwsh.exe / python.exe / pythonw.exe / cscript.exe / wscript.exe / svchost.exe / services.exe`（计划任务走 `svchost.exe`，服务走 `services.exe`）。
  · **验证（两条路径都要跑，缺一不可）**：① **自动化路径**：`& dist\SysRecoverCheck.exe --quick <img>` → rc=0 **0 秒**；无参 → rc=2 **0 秒**（`secs` 用 `Get-Date` 差值量，停在那儿等回车就会 >0）；`make smoke-cli` **24 项 PASS**。② **双击路径**：宿主 shell 里 `Start-Process` 的父进程是 `powershell.exe`（命中宿主名单，**永远测不出发弹框**）→ 必须用 **`& explorer.exe <exe>`** 让父进程变成 `explorer.exe`；实测 `processes=2`（pid=7536 parent=1504/**explorer**、pid=8384 parent=7536/**SysRecoverCheck**）→ 子进程窗口表里有 `460956|#32770|vis|选择要检查的系统镜像` → `PostMessage(WM_CLOSE)` → **`left after close=0`**（启动器与子进程都退出）。
  · **门禁**：`make check` 61 用例/532 断言 + check-docs PASS；`make package` 双架构（`0.9.42`）+ `make sha256`（352 文件）+ `dist-check` + `smoke-cli` 24 项全绿。
  · **教训**：① `CreateProcessW` 的**控制台继承**会让"子进程独占控制台"的判据失效 —— 跨进程传"这个控制台归你"这种**上下文**，用环境变量最省事（无需改命令行、无需 IPC、不会被参数解析吃掉）；② 判"是不是无人值守"**不能只看控制台进程数**，还要看**父进程是谁**（PowerShell 脚本里子进程同样独占控制台）；③ 自动化验证"双击行为"**必须换父进程**（`explorer.exe <exe>`），`Start-Process` 模拟不出来 —— 否则会得出"修完还是不弹框"的假阴性。

### PIT-183

- PIT-183 **「镜像检查」确认框标题只显示半句、右边超出外框：`Ask2` 的 title/msg 传反了 —— 整段 3 行提示进了只画单行的标题标签**（2026-10-27 用户实测，`0.9.43`）：
  · **现象**（用户原文）：点「更多功能M>> → 镜像检查（完整校验）」后，弹框标题栏只有「将对整个镜像做完整性校验（逐字扫描）。几十GB的镜」，**右边超出外框看不到**；正文位置却显示「镜像检查」。
  · **根因**：`CConfirmDlg::Ask2(owner, title, msg, leftText, rightText, defaultIsRight)` —— 五个参数里 title 与 msg **同为 `const std::wstring&`**，位置相邻、类型相同，编译器不会报错。`main_form.cpp::StartVerifyImage()` 把**整段 3 行提示写成了 title**、把「镜像检查」写成了 msg。而 `skin/confirm.xml` 的 `DlgTitle` 是 **22,12,300,38 的单行标签**（`CSkinLabelUI` 走 `TextOut`，不认 `'\n'`、超宽直接画到框外）→ 三行叠在一行高度里、第 300px 之后的字全被截。
  · **修法（调用点 + 公共防御，两层）**：
    ① 调用点交换参数：`title=Tr(L"镜像检查")`、`msg=`三行提示；行内补注释写明「title 只能单行」与本次事故原文（防止下次又照位置抄）。
    ② `confirm_dlg.cpp` 新增 **`NormalizeTitleAndMsg()`**（`RunModal` 开头调用）：title 里出现 `'\n'` → 首行留作标题、其余行**并回正文前面**，并 `LogWarn("confirm dlg: multi-line title downgraded into message (fix the caller)")` —— 即便有人再写反，界面也退化成"标题+多行正文"的正确形态，且日志点名要修调用方。
    ③ 同批新增 **`ClampMsgLines(msg, 6)`**：skin 只有 `DlgMsg1..6`，此前第 7 行起**静默丢弃**；现在截到 6 行并在末行加 `...` + `LogWarn`，让"还有内容没显示"在界面上可见。
  · **全量复检**：`Ask2/Ask3/Ask4` 全部调用点逐一核对（`main_win.cpp` 380/536/615；`main_form.cpp` 852/857/894/898/917/967/990/1005/1303/1515/2189/2222）——**无第二处多行 title**。
  · **验证**：`make check` **61 用例/532 断言** + check-i18n **650/650**（未新增词条）+ docs/widths/shell PASS；`make package` 双架构 + `make sha256`（352 文件）+ `dist-check` PASS。**GUI 自动化实测**（`verifydlg.ps1`：进程内拖入 `D:\ea-lab\ea.wim` → 点 MoreBtn(645,314) → 菜单取 rect/4 算第 4 项 → `SetCursorPos`+`mouse_event`）→ 确认框 **420×196**，标题单行「镜像检查」、三行提示完整、按钮「取消 / 开始校验」不被裁（截图为证）。
  · **自动化踩坑（下次直接抄）**：① PS 5.1 里 `[UIntPtr]` 转换会失败（`GlobalAlloc` 的 `size` 只有声明成 **`uint`**、返回的 `IntPtr` 直接当 wParam 传给声明为 `IntPtr` 的 `PostMessageW` 才通）；② `#32768` 菜单不吃 `PostMessage`，必须真实鼠标事件；菜单项纵向位置可用 `(bottom-top)/项数` 估；③ 主窗口类名 `SysRecoverUI`、确认框 `SysRecoverUI.Confirm`，用 `EnumWindows`+`GetClassNameW` 找（`FindWindowW` 传 `$null` 在 PS 5.1 不可靠）。
  · **教训**：① 相邻同类型参数（`title, msg`）**必须在调用方写行内注释**，否则"位置写反"是零成本事故；② 公共对话框要**防御调用方**（归一化 + 截断 + 留日志），不能假设每个调用点都记得约束；③ 界面上"少了一半字"优先查**控件宽度/单行绘制**，不要先怀疑字体或 DPI。

### PIT-184

- PIT-184 **极简 Win8PE「分区与 BIOS/UEFI 引导全部免 bootsect / bcdboot / bcdedit」：随包资产自足（`0.9.44`/`0.9.45`，2026-10-27/28）**：用户反馈 2 原文——极简 Win8PE 里「传统（MBR）分区后提示缺少一个分区相关文件并中止（但分区其实已完成并激活）；再次进软件可正常装系统；装完又提示缺少文件、无法做引导恢复，用镜像里的 `fixboot.exe`（918KB）修好」。逐层核实后**三处**都依赖系统工具，全部改为随包资产：
  · **① 分区向导**（`src/disk/partinit.cpp` + 新 `src/disk/mbrcode.{h,cpp}`）：旧代码在 diskpart 全部成功之后才检查 `bootsect.exe`，缺失就 `return 1` → 用户看到"分区失败"而盘其实已建好并激活。现在优先用随包 `bootfiles\mbr-boot-code.bin`（440B）写磁盘**前 440 字节**（保留 446..509 分区表与 510..511 的 `55AA`；磁盘已有非零代码则**保留**，不破坏 GRUB/第三方引导），`bootsect` 只在资产缺失时兜底，两者都不可用**只 WARN 不中止**；回读校验（代码 + 活动分区）失败也不再让已完成的分区操作报错。**写磁盘用 `\\.\PhysicalDriveN` 在偏移 0 写 512B，实测可写**（磁盘 MBR 区不属于任何已挂载卷）。
  · **② 自足引导模块**（新 `src/boot/bootself.{h,cpp}`）：
    - **NTFS 分区引导区**：`ntfs-boot-code.bin`(426B) → offset `0x54`，`ntfs-boot-cont.bin`(4096B) → offset 512（扇区 1..8）。先读扇区 0 判断 0x54 是否已是 `fa 33 c0`，是就不写（见下面"坑 2"）；4Kn（逻辑扇区 >512）跳过并 WARN（微软不支持 4Kn 的 BIOS 引导，PIT-133）；非 NTFS（OEM 名不是 `NTFS    `）跳过。
    - **BIOS**：`\bootmgr`（源：`<目标>\bootmgr` → `<win>\Boot\PCAT\bootmgr` → 本机 `%windir%\Boot\PCAT\bootmgr` → 本机 `\bootmgr`）+ `\Boot\BCD` ← 随包 `bootfiles\bcd\bios.bcd`（**完全静态**：`{bootmgr} device boot` + `path \bootmgr`；`{default} device/osdevice boot` + `path \Windows\system32\winload.exe`；无 resume 对象，机器无关）+ 字体/资源/语言目录/boot.sdi（尽力复制，缺则 WARN）。
    - **UEFI**：`\EFI\Microsoft\Boot\*`（把 `<win>\Boot\EFI` 整棵镜像过去，等价 bcdboot 产物）+ `\EFI\BOOT\BOOTX64.EFI`（固件兜底路径）+ `\EFI\Microsoft\Boot\BCD` ← 随包 `bootfiles\bcd\uefi.bcd`，运行时把**两处 16B 分区 GUID 占位**替换成真实值（ESP 5 处 / 系统卷 10 处；GUID 的内存布局 = `IOCTL_DISK_GET_PARTITION_INFO_EX` 的 `Gpt.PartitionId`）+ 固件 `Windows Boot Manager` 条目（`uefi::EnsureWindowsBootEntry`：短格式 HardDrive + FilePath，挂 BootOrder 末尾，写后读回，失败只 WARN）。
    - **顺序有讲究**：先镜像 `<win>\Boot\EFI` 再补 `bootmgfw.efi` —— 反过来会被镜像里的同机版本覆盖（实测 3,086,848B 被 1,588,608B 覆盖）。
  · **③ 接线**（`src/app/ops.cpp`）：就地还原（BIOS 与 UEFI）、`repair-boot`（两分支）、暂存的 UEFI 段 —— **bcdboot 可用时照旧**（它还会补 MUI/固件项），**缺失或失败自动回落自足路径**，仍做产物断言（`VerifyEspBcd` / `BiosBcdLooksValid`）。`BiosBcdLooksValid` 在无 `bcdedit.exe` 时改**文件级判定**（BCD 里 UTF-16LE 出现 `winload`）；`bootfix::PrepareBootFixFiles` 改用随包模板 + 目标系统自己的 `bootmgr`（不再 `bcdedit /export`，本机 UEFI-only 安装没有 `\Boot` 也能出 bootfix）。**保留**"BCD 已有效则跳过重建"（PIT-119 第三方启动项）。
  · **两个 Windows API 坑（都是本轮实测踩到的，务必记住）**：
    - **坑 1：卷句柄裸读写必须 OVERLAPPED + 扇区对齐**。`\\.\X:` 上用 `SetFilePointerEx` + `ReadFile/WriteFile` 会**失败且 `GetLastError()=0`**（读都是 0）；直接用 `FileSeek(0x54)` 写 426B 因非扇区对齐也必失败。正解：`OVERLAPPED{Offset,OffsetHigh}` 显式给偏移 + 整扇区读改写（读 512 → 改 0x54 处 426B → 写 512）。
    - **坑 2：已挂载卷的 VBR（第一扇区）被 Windows 拒写** —— `WriteFile` 返回 `ERROR_ACCESS_DENIED(5)`，`FSCTL_LOCK_VOLUME` 成功、`FSCTL_DISMOUNT_VOLUME` 成功**也照样拒写**；而写扇区 1..8 正常。所以 PBR 逻辑改成"**先校验、只在需要时写、写不进只 WARN**"：正常路径 `format.com`（Windows）与 `mkntfs`（Linux 侧，PIT-053 已有自己的写回）本来就写微软代码，不触发；真需要强写时提示 `bootsect /nt60 <盘>: /mbr`。
  · **验证（QEMU，两个固件都到桌面）**：① 模块级 lab `tools/bootself-lab.cpp`（`mbr|zerombr|dump|pbr|zero|bios|uefi` 模式，不进产品）在真实 Windows 卷上生成引导树，`bcdedit /store` 回显 `{bootmgr} device partition=Y:`、`{default} device/osdevice partition=T:`（GUID 定向正确）；② **BIOS 自足全链**：清掉 `\bootmgr`/`\Boot\BCD` 后由随包资产重建 → SeaBIOS 启动到桌面；③ **UEFI 自足全链**：ESP 只用随包资产（含 `\EFI\BOOT\BOOTX64.EFI` 兜底）→ OVMF 启动到桌面（同一镜像挂 virtio 控制器时蓝屏 `INACCESSIBLE_BOOT_DEVICE` 属控制器驱动缺失，不是引导链问题）；④ MBR 代码"清零 → 写入 → 回读"通过（头 8 字节 `33 C0 8E D0 BC 00 7C 8E`，`55AA` 保留）。两个 BCD 模板的**可启动性**此前已单独在 QEMU 验证（BIOS/SeaBIOS 与 UEFI/OVMF 各一次）。
  · **回归**：`make check` **63 用例/542 断言**（新增 `bcd_patch_placeholder_guids`、`bcd_file_looks_usable`）+ check-i18n **642/642** + docs/宽度/shell/layers/secrets PASS；`make package` 双架构 + `make sha256`（**357** 文件，新增 5 个资产）+ `dist-check` PASS。（i18n 键数从 650 降到 642：删掉了"缺 bootsect/缺 bcdboot 就失败"那一批已不存在的文案。）
  · **体积**：+5 个资产（440B + 426B + 4096B + 20KB + 36KB）≈ **61KB**，符合"不新增驱动、体积 +0.1~0.3MB"的约定。
  · 🚧 **待真机**：极简 Win8PE 全链（分区 → 还原 → 引导修复）。

  · **⚠️ 2026-10-28 更正（火绒实测，把 PBR 归因与顺序都改对了）**：用户在开发机上发现火绒日志
    `防护项目：写磁盘保留扇区 / 执行文件：VOLUME / 操作结果：已阻止`，指向我们自己 lab 工具的 VBR 写入。
    逐条实测的结论：
    - **不是 Windows 拒写**（PIT-184 初版的"已挂载卷 VBR 被 Windows 拒写"是**误判**）：是**安全软件**拦的。
    - 同一台机器、同一扇区，**`bootsect /nt60 L:`（微软签名）rc=0 成功**；而**我们自己的进程**三条路
      **全部 `ERROR_ACCESS_DENIED(5)`**：① 卷句柄 `\\.\L:` 写；② `\\.\PhysicalDrive3` 按分区偏移直写；
      ③ 先 `FSCTL_LOCK_VOLUME` + `FSCTL_DISMOUNT_VOLUME` 再物理盘直写。→ **AV 是按进程拦的，签名工具才放行**。
    - 因此 `EnsureNtfsBootRegion` 改为**四段式**（用户 2026-10-28 规格："先用系统自带功能，再用我们自己的"）：
      ① **只读检查**（0x54 是否 `fa 33 c0` **且** 续段有实质内容，续段前 512B 非零 ≥64）→ 都正常就**一个字节都不写**
      （不惊动安全软件、不弹提示；`format.com` 正常写过后走的就是这条）；② 有缺陷 → **先跑 `bootsect /nt60 X:`**
      （微软签名，AV 放行；它会把代码段+续段**整对**重写，避免"系统工具写一半 + 我们补一半"的**混写**风险 ——
      426B 与扇区 1..8 是配套的一对，`jmp 0x226` 跳进续段）；③ 仍不行 → 自足写（卷句柄 → 物理盘兜底，**整对写**）；
      ④ 还是不行 → 日志 `pbr: BLOCKED by security software…` + `repair-boot` 的用户提示里明确"临时退出火绒/360 的
      『写磁盘保留扇区』保护（或把本程序加入信任）后重试，或在 PE 里修复"。**平时不弹这类提示**（只有真失败才提示）。
    - 实测链路（QEMU）：`qemu-io` 在**卸载状态**把 L: 的 0x54(426B) 与扇区 1..8(4096B) 清零 → 挂回 → 我们的流程输出
      `pbr: needs repair (code=bad cont=ok)` → `bootsect /nt60 L: rc=0` → `pbr: ok via bootsect (code=ok cont=ok)` →
      **SeaBIOS 启动到桌面**（截图 `<lab>\sfboot\bios-bootsect.png`）。即"系统工具优先"这条路已端到端验证。
    - **物理盘直写兜底保留但不指望**：实测在"已挂载卷的元数据区"上它也被拦（err=5），只在"卷没有盘符/已卸载"等
      场景才可能有用；日志会如实写 `phys: layout unavailable`/denied，不再声称"物理盘一定行"。
    - `repair-boot` 的 BIOS 分支：PBR 仍写不进时**不再假装成功** —— `msg` 追加一句可执行的处置（用户可见），
      返回值仍是 0（引导文件已写好），细节进日志。

### PIT-185

- PIT-185 **被安全软件拦写时给「重试」而不是让用户猜：结构化 blocked 标志 + GUI 重试框（`0.9.47`，2026-10-28，用户规格）**：用户口径——"用「修复引导」或「安装」被火绒拦下后，你提示，我退出火绒，点你弹出的重试后成功，那就没什么问题了"。所以**不是**把提示减到没有，而是"**只在该拦的时候弹，且弹出的框里带重试**"：
  · **ops 层（不靠解析文案）**：`thread_local bool t_bootBlocked` + `ResetBootBlocked()/TakeBootBlocked()`；置位点 = `BootWriteBlockedHint()`（BCD/固件变量写失败）、`RepairBoot` 的 PBR 写不进、`EnsureNtfsBootRegion` 失败。**必须同一线程 Reset/Take**（工作线程里取，主线程只用布尔成员）——否则取不到。
  · **GUI**：`StartRepairBoot(bool confirm = true)`（`confirm=false` 供重试用，不再重复确认框）；worker 开头 `ResetBootBlocked()`、结束 `TakeBootBlocked()` 存 `m_fixBlocked/m_menuBlocked`；`WM_FIXBOOT_DONE` 与 `OnMenuDone` 里**只有** `blocked` 为真才弹 `CConfirmDlg::Ask2(标题, 3 行提示, 「关闭」/「重试」, 默认=重试)` → 点重试**直接重跑**同一条操作（修完火绒防护后一次就过）；普通失败仍走原来的信息框（不打扰）。
  · **文案几何**：提示压成 **3 行、每行 ≤24 字**（15/18/24）—— `CSkinLabelUI` 单行绘制、超宽会被裁（PIT-183 教训）；3 行对话框 420×196、按钮完整可见。中英各一条（`lang` 648 keys）。
  · **与 PBR 四段式的关系**：正常路径（引导区已正确 / `bootsect` 成功）根本不会置位 → **不弹任何框**；只有"`bootsect` 也没有、我们自己也写不进"（极简 PE + 安全软件这种组合）才会走到重试框。
  · 🚧 **待实测**：真被拦时弹框出现、退出火绒后点「重试」一次成功（本机引导区已正常，不会触发；等用户/客户现场验证）。

### PIT-186

- PIT-186 **「安装到启动」改三选（扫描/解压/取消）+ 顺手挖出潜伏 bug：皮肤 XML 的 `size` 会把运行时加高的对话框压回 196，4/5/6 行消息一直是"按钮被裁"**（`0.9.48`/`0.9.49`，2026-10-28，用户规格）：
  · **需求（用户）**：点「安装到启动」时不该**立即解压**，而应先给两个选项——「扫描」= 只检查镜像能否**只靠 Linux 救援层**还原（不占 C 盘）；「解压」= 现在就解压到 C 盘备用；下方三个按钮 `扫描 / 解压 / 取消`，默认「扫描」。
  · **实现**：
    - `ops::CanRestoreInLinuxOnly(image, index, &badReparse, &encrypted, err)`：复用 `WimEngine::ScanImageForLinuxRestore`（**只读元数据、秒级、不动盘**，与 block 引擎的 F5/W1/W2 预检同源），返回"能否免解压"。
    - GUI `ToggleBootMenu`：`CConfirmDlg::Ask3(标题, 5 行, 「扫描」「解压」「取消」, defaultIdx=0, midDisabled=false, escIdx=2)`；**回车=扫描、ESC=取消**。选「解压」→ `StartMenuToggle(true, "swap")`（原行为）；选「扫描」→ `StartMenuScan()` worker（`WM_MENU_SCAN_DONE`=WM_APP+26）→ 干净则 `StartMenuToggle(true, "block")`（安装时**不解压**，还原时由救援层直接写入）；不干净则弹 `Ask2`（说明原因：特殊重解析点 / EFS 加密文件，默认=解压）。
    - `StartMenuToggle(bool install, const std::string& engine = "swap")` 增加 engine 形参（原先**写死** `"swap"`，即"安装即解压"）；日志记 `engine=`。
    - `Ask3` 新增 `escIdx`（默认 0，保持历史语义）：本对话框 left=扫描是**动作**不是安全项，ESC 必须落「取消」→ 传 2。
  · **⚠️ 顺带挖出的潜伏 bug（`0.9.49` 修）**：`RunModal` 明明按行数算了高度（5 行 = 240）传给 `Create`，但 **Duilib `WindowImplBase::OnCreate` 会按皮肤 `skin/confirm.xml` 的 `<Window size="420,196">` 把窗口尺寸重置回 420×196** → `InitWindow` 里按钮按 5 行排到 y=180，窗口仍是 196 → **按钮下半截被裁**（截图实证）。3 行消息恰好等于 196，所以 2026-10 之前从没暴露；**PIT-158 的"空间不足"5 行框、PIT-159 的 6 行框、Ask4 四选框同样中招**（用户之前看到的"按钮只露上边"就是它）。**修法**：`InitWindow` 末尾按 `MsgLineCount(m_msg)` 再 `SetWindowPos(m_hWnd, …, w, h, SWP_NOMOVE|SWP_NOZORDER)`（`w` 四选=520、其余 420）；并加日志 `confirm dlg: lines=N h=H msgLen=L` 便于日后核对。
  · **验证（GUI 自动化截图）**：`D:\ea-lab\sfboot\menu-dialog-shot3.ps1`（剪贴板 + 点击输入框粘贴镜像路径 → 点「安装到启动」(client 506,316)）→ 确认框 **420×240**，5 行完整（用法/镜像/目标会格式化/「扫描」说明/「解压」说明），三按钮 `扫描`(蓝底默认) / `解压` / `取消` 全部完整（`menu-choices.png`）。
  · **自动化踩坑（可复用）**：① `Add-Type` 里 `[DllImport("user32.dll")] GetClassNameW` 必须写 **`CharSet=CharSet.Unicode`**，否则返回的类名只剩首字母（"S"），永远匹配不上 `SysRecoverUI`；② 主窗口可能被上次运行留在屏幕右/下边缘（离屏）→ 先 `SetWindowPos(main, 60,40,745,410)` 再点，否则对话框出屏；③ 截图用 `PrintWindow(hwnd, hdc, PW_RENDERFULLCONTENT)`，`CopyFromScreen` 在被遮挡时抓到的是别的东西；④ `C#` 里静态方法别叫 `Main`（会被当入口点，`CS0017`）；⑤ 单实例互斥会拦下新实例 → 跑之前先 `Stop-Process` 掉残留 `SysRecover*`。
  · 🚧 **待实测**：① 老镜像（含 EFS/特殊重解析点）选「扫描」→ 应提示改用「解压」；② 干净镜像选「扫描」→ 免解压安装后**开机选 SysRecover 能还原**（本条没自动测，因为安装会写本机 C: 的引导文件）。

### PIT-187

- PIT-187 **AGENTS.md 涨到 422 KB（§13 PIT 正文占 88%）→ 正文迁 `docs/25` + 只留活跃索引 + 体积闸门（`0.9.50`，2026-10-28；用户要求"先备份，请专家组精简"）**：实测 422,671 B / 1,530 行，其中 §13（坑位登记册）**372 KB = 88%**、186 条 PIT；每次会话都会被整篇注入上下文。两位专家（文档架构 / 工程门禁）并行评审后按"搬家 + 索引化 + 去重"落地：
  · **备份**：`docs/老旧文档暂存/AGENTS-2026-10-28-精简前.md`（逐字节相同，作为集合/正文对账基线）。
  · **正文原样迁** `docs/25-PIT坑位册.md`（每条加 `### PIT-NNN` 标题做锚点；**编号与正文一字不改、不重排** —— 全仓 626 处代码/文档注释只写编号，改号即全部失联）。
  · **§13 只剩活跃索引** 63 行（铁律 8 / 未闭环 🚧 38 / 近期 0.9.x 17），行格式固定 `- PIT-NNN · ≤30 字结论 · [正文](…#pit-nnn)`；并写明"改 `ops.cpp`/`zjrestore-lite.sh`/`bcd.cpp`/`uefi.cpp`/`bootself.cpp`/`main_form.cpp` 前先 `grep PIT-` 再查 docs/25"。
  · **顺手修的既有瑕疵**：`PIT-066` 重号（第二条标题标注"（二）零注册正解（现行）"，避免锚点撞车）；`PIT-136` 空号补记（消除 `docs/20` 与 8 处代码注释的悬空引用，内容指向 PIT-135/157）；18 处 `AGENTS.md §13` 引用改指 docs/25；`docs/11` 的"PIT-001~117"、`docs/12` 的"71 条"、AGENTS 顶部硬编码版本号（0.9.39）全部校正。
  · **门禁加固**（`tools/check-docs.py`）：① 把 `docs/25` 加进 `DOCS_CONTRACT` —— 否则正文搬走后 `contract_version` 命中从 3 处降到 1 处，**静默变松**；② 新增 **`AGENTS.md ≤ 80 KB`** 体积闸门（超了直接 FAIL，逼新坑写 docs/25）。
  · **校验（机器化，不靠"看起来对"）**：186 条编号集合前后一致（无缺无多）、63 条索引链接都有对应 `### PIT-####` 锚点、`contract_version` 在 AGENTS 仍有 1 处命中、`make check` + `make package` 全绿。
  · **结果**：AGENTS.md **422.7 KB → 56 KB（-87%）**；docs/25 367 KB 不随会话注入、按 `#pit-nnn` 检索。
  · **教训**：① 文档也是上下文成本 —— PIT 属"按需检索"资产，不该常驻；② 搬家前**逐字节备份**，否则无法证明"没丢"；③ 迁移判据是"编号/正文一字不改 + 集合相等 + 锚点可达"；④ **门禁的文档清单要跟着搬**（DOCS_CONTRACT），否则覆盖面静默缩小。

## 附录 A：非坑位记录（新功能/版本推进/勘误）

- 📌 **编号勘误（2026-09-20）**：§13 里有**两个 PIT-066**（前者"两种走法对比"、后者"零注册正解（已实测通过）"，是同一主题的两次记录）；代码注释与 §7/§14 引用 `PIT-066` 时指的是**后者**（Canonical 签名链）。暂不改号以免破坏交叉引用，日后整理时合并。

- ✨ **新功能（2026-09-20，用户在笔记本上实现）**：GUI 第一步支持**把镜像文件拖进窗口**（`.esd`/`.wim`），等价于点「浏览系统镜像文件」→ 设为还原镜像。涉及 `src/gui/{main_form,ui_skin}.{h,cpp}`。

- ✨ **备份信息与默认文件名（2026-09-21 用户规格）**：备份模式下——
  · **文件名下方（备注框）预填**「`日期 + 系统类型 + 备份`」，例如
    `20260921 Windows 10 IoT 企业版 LTSC 21H2 19044.4046 备份`（仅当用户尚未手输时预填；该值同时成为 WIM 里的子镜像名）；
  · **默认文件名**改为 `20260921Win10.19044备份.esd`（`日期 + Win<家族>.<build> + 备份`；保存对话框与自动命名两处都改了）。
  · 实现：新增 `src/common/sysinfo.{h,cpp}` 的 `DescribeRunningSystem()` —— 读
    `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion` 的 ProductName / EditionID / DisplayVersion /
    CurrentBuildNumber / UBR 组合。**版别名走 `EditionID → 中文映射`**：实测有的系统 ProductName 是英文且带年份
    （"Windows 10 IoT Enterprise LTSC 2021"），而我们要的是"Windows 10 IoT 企业版 LTSC"；另 Win11 的
    ProductName 仍写 "Windows 10"，家族号以 build ≥ 22000 纠正。

- ✨ **备份/恢复 ESP 分区（2026-09-30，方案 C「单文件双子镜像」，`0.6.0`，论坛 66 楼用户拍板）**：备份时可选把 ESP 作为**同一镜像的第 2 个子镜像**（子镜像名 `ESP` + 英文 description），还原系统时按契约 `esp_index` 从**同一个 `.wim/.esd`** 恢复到 ESP 分区。⚠️ 早期 A 方案（同目录同名 `.esp` sidecar，`0.5.3`）已废弃、实现已删除；导入时用户调研后拍板改 C（单文件传输/校验/复制都只管一个文件）。
  · **入口**：GUI 备份模式第三步 `EspChk`（`skin/main.xml`，复用隐藏的 SearchBtn 位 639,84,710,109，还原模式隐藏；无 group 属性 = 独立开关，与 `Silent` 互不取消）、CLI `backup --esp`；默认不勾选；悬停提示写明"写入镜像（子镜像名ESP）/ 恢复系统时自动恢复回 ESP"。
  · **原子性（`ops.cpp::RunBackup`）**：`stage = esp && !append`；主镜像先写 `<dest>.stage`（Capture 原子 tmp）→ esp 并入（新建 `WimEngine`，`Append(..., L"ESP", false, EnsureEspExclusionConfig(), …, kEspImageDesc)`；`ProgressUpdate("backup",100,"esp")`）→ `MoveFileEx(REPLACE|WRITE_THROUGH)` rename 到 `<dest>`；erc≠0 时先 `Probe(stage)`：坏 → 删 stage 报"主镜像写入损坏，已删除，请重新备份"，好 → 照常交付再报错。append 模式不 stage（原地追加，erc==-1 分支保留），失败报"（镜像写入损坏，请重新备份）"；esp 并入失败 tail = "（主镜像已生成，但未包含 ESP 备份）"；rename 失败 = `备份写出失败：无法提交镜像文件(err=N)`。无 ESP 仅 `LogWarn("esp backup skipped")` rc=0；失败路径不 AppendHistory（与既有 capture/probe 失败一致）。
  · **契约（增量键，`contract_version` 保持 1）**：`esp_index`（0/缺省=没有）写进 `restore-task.conf`（`image_index` 后）+ `restore-task.json` + `_zjresy*.log`；救援层早期 `ESP_INDEX=$(get_task esp_index)` + `case ''|0 → 置空`，另打一行 `say "esp subimage: index=…"`。老任务缺键=跳过、老救援层多键=忽略 → 双向兼容（`ZJ_CONTRACT` 不动，check-docs 仍 PASS）。
  · **还原防呆（`StageRestoreImpl` 步骤 0.5，Probe 之后、一切写盘之前）**：`ListImages` 后名字含 `L"ESP"`：① **== 还原目标 index**（eff = `req.index<1?1:req.index`）→ `err = Tr("子镜像「")+W2U(d.name)+Tr("」是 ESP 分区备份，…请选择系统子镜像。")` + `AppendHistory("restore-rejected", …, "esp image selected as restore target")` + **return 4**（CLI 透传 rc=4；GUI 走统一失败提示）；② 否则取首个含 ESP 的 index 当 `espIndex`（**从镜像内容发现，不依赖备份时是否勾选 --esp**），随任务写契约。`extract` 不拦（按路径提取是合法用途）。ListImages 失败仅 LogWarn、espIndex=0。
  · **恢复侧（`zjrestore-lite.sh`，改后已 `build-debian-rescue.py` 重建，494 模块/20.5MB 不变）**：sidecar 发现段整体删除；`find_esp_dev()` 不变（GPT 类型 GUID `c12a7328-…` 优先 → FAT+根有 `\EFI` 回退，跳过 TARGET/loop/ram）→ mount → **`$WIMLIB apply "$IMG_FILE" "$ESP_INDEX"`**（挂载中的镜像分区直读；`umount "$SRC_M"` 从原位置移到 ESP 块**之后**）→ 目录模式 apply 只新增/覆盖、不删 → 失败仅 `say ERROR` 不中断主流程。
  · **就地还原侧（`RunDirectRestore`，2026-09-30 用户指明补齐）**：PE / 还原到非系统盘**不走救援层**，原来这条路径漏了 ESP 恢复（tooltip 承诺"恢复系统时自动把ESP一并恢复"不成立）。补法：主 apply 完成后、**bcdboot 之前**，按 `espIndex`（0.5 步已从镜像内容发现）对 `AcquireEspRoot` 找到的本机 ESP 做目录模式 apply；**空进度回调**（`ProgressFn{}`，`ProgressThunk` 对空回调安全）不回卷已到 100% 的进度条；**非致命**——失败或找不到 ESP 只 `LogWarn`，主系统已还原不动摇（对齐救援层 `say ERROR` 不中断先例）；bcdboot 放在最后写、保证引导文件权威。BIOS 机器通常无 ESP → 自然跳过。
  · **⚠️ 坑（实测）**：① **`esp capture rc=47 Failed to open a file` 的真凶是 `\EFI\Microsoft\Boot\BCD`（+`.LOG*`）被运行中的 Windows 当 hive 挂着（`HKLM\BCD00000000`）独占锁**（PIT-051 同源）——**不是** `System Volume Information`（它读得到，A 方案时归因错过一次）；BCD 必须进排除清单（还原后由流程内 `bcdboot` 重建，PIT-090；且 apply 不删 → 现役 BCD 安全）。② wimscript 排除目录**不能带尾斜杠**：`\System Volume Information\` 的 glob 不匹配任何东西（wimlib/DISM 语义 = 匹配目录条目本身即跳过整棵），必须照 `exclude.cpp:26` 写 `\System Volume Information`。③ 排除配置走临时 `zjrestore-esp-exclusion.ini`（同 `EnsureExclusionConfig` 做法）；`wim.h` 不外泄 `wimlib.h` → 未初始化错误用 `-1` 特判。④ **wimlib 1.14 没有 `wimlib_get_image_count`** → 新子镜像 index 用 `wimlib_get_wim_info().image_count`；description API 拼写是 **`wimlib_set_image_descripton`**（少一个 i）。⑤ GUI 取消清理 `CleanupIncompleteOutput` 的候选路径要含 `<dest>.stage` / `<dest>.stage.tmp`（**绝不能删最终路径**：stage 已 rename 交付的主镜像是有效产物）。
  · **验证（全过）**：`make check` **31 用例/208 断言** + 341 i18n keys + check-docs PASS；`make package` 双架构；**Windows 真机冒烟**：`backup --esp` → rc=0、单文件 12.4MB、**无 `.esp`、无 `.stage/.stage.tmp` 残留**、`images` = `1 | Backup` + `2 | ESP`（英文 description 正确）；`--append --esp` → 4 子镜像（Backup/ESP/Backup(2)/ESP(2)）；`restore --index 2` → **rc=4 + 拒绝提示**、零契约写入（无 restore-task.conf）、history 有 `restore-rejected`；**QEMU 端到端演练**（`mk-drill.py` 改造：conf+log 带 `esp_index=2`、sda1 兼作假 ESP——根下 `EFI/` 命中 find_esp_dev 的 FAT 回退 + 预置 `EFI/DRILL-MARKER.txt` 验证只加不删、`test.wim` 用 `--esp` 重建）：`restore esp subimage idx=2 -> /dev/sda1` → `esp restored (rc=0)` → `RESTORE DONE: /dev/sda2 (index 1)` → `reboot: Restarting system`；盘上复核：**marker 幸存**（只加不删 ✓）、`EFI/Microsoft/Boot/bootmgfw.efi` 3,087,872B 落进假 ESP、`BCD` 不在（排除生效 ✓）。🚧 GUI 勾选框外观/文案待用户实测；UEFI 真机全链（暂存→重启→恢复 ESP 子镜像）待回归；**就地还原侧 ESP 恢复（PE/非系统盘）待实测**（就地还原本身此前就标"待实测"，见 PIT-064）。✅ 2026-09-30

- **版本推进（2026-10-21）**：分区向导完成后版本定为 **0.8.0**（`version.py --set 0.8.0`，initramfs stamp 0.8.0）；下一任务 **异机还原**（还原到不同硬件/控制器），完成后进入 **0.9**。
  · **教训**：① "只推荐不禁止"与"技术硬限制"要分开——用户选择权优先，但 >2TB/MBR 与 Win7 x86/UEFI 是物理/系统限制；② 新 MBR 盘必须补磁盘引导代码（本项目的还原流程只写 PBR，PIT-051/053）；③ 空下拉直接调用向导比"提示项/额外按钮"更简洁（用户拍板）；④ 新增含 Tr 的源文件**必须同时加 WRAP_FILES**。

### PIT-188

- PIT-188 **镜像加密（ZJENC1）与增量备份落地：wimlib 只能按路径打开 → 加密镜像必须整包解到临时明文；ESP/EA 子镜像改由 Windows 暂存**（`0.9.53`，2026-10-29，用户拍板"全部按建议执行"）
  · **需求**：①「对镜像实施加密码」——一个口令护全镜像；②「WIM/ESD 增量备份」；③ 两者本轮**不做组合**。
  · **加密（`src/common/crypt.{h,cpp}`）**：容器 = 明文头（magic/版本/KDF/salt/块大小/明文长度/头 MAC）+ 加密 meta（子镜像清单）+ 1MB 分块正文。
    口令 → `PBKDF2-HMAC-SHA256(600k)` → 64B（encKey+macKey）；正文 `AES-256-CTR`（counter = 块号 ‖ 块内计数，用 ECB 生成密钥流），每块 `HMAC-SHA256`（encrypt-then-MAC）；错口令/篡改/截断一律 fail-closed，且**不留半个明文文件**。
    **不用 AES-GCM**：Win7 的 CNG 不保证支持 GCM；CTR+HMAC 是同等强度且 Vista/Win7 可用（`bcrypt.dll`，`-lbcrypt`）。
  · **⚠️ 硬约束（这条决定实现形态）**：wimlib 只有 `wimlib_open_wim(path)`，**没有 fd / 自定义 I/O 回调**，而且按资源表**随机访问** →
    加密镜像无法"边解边喂"；只能先解密成**同卷临时明文**再交给 wimlib，用完立即删（`app/imgsrc.cpp::ResolveImagePlain`，进程内按路径缓存一次）。
    代价：`verify`/`extract`/`A 引擎解压`/PE 就地还原需要**与镜像等量的临时空间**；`backup` 加密不额外占（明文本来就存在 `<目标>.plain.tmp`，加密完删）。
  · **meta 为什么必须加密存在容器里**：GUI 选镜像、`images`、空间预估、winload 预检原来都要开 WIM；加密后若每次都整包解密，选中一个 20GB 镜像要等几分钟。
    meta 存 `{index,name,description,sizeBytes,creationTime,hasWinload}` → 只解 1 块就够（`ZjeReadMeta`），其余走 `ImageList/ImageContentSize/ImageHasWinload`。
  · **引擎矩阵（用户确认）**：A 引擎（解压并安装 / 还原后重启）+ PE 就地还原 ✅；B 引擎（block，Linux wimlib apply）❌ `rc=4`；「安装到启动」常驻菜单 ❌ `rc=4`（开机阶段没人能输密码）。
    `StageRestoreImpl` 在最前面统一拦（在一切写盘之前），GUI 在「安装到启动」点击时也先拦并说明。
  · **ESP/EA 子镜像（加密镜像专用改道）**：原来由救援层 `wimlib apply`（它读不了密文）→ 改为 **Windows 暂存阶段解到 `~new\ZJRESTORE\{espfix,eafix}`**，
    契约新增增量键 `esp_fix_dir` / `ea_fix_dir`（`esp_index`/`ea_index` 置 0），`zjrestore-lite.sh` 只做 `cp -a`（挂目标盘拷贝）。
    非加密镜像**保持原行为**（Linux 自己 apply），不动已验证路径。
  · **增量（`0.9.53`，docs/31）**：GUI「更多功能>>」→「增量备份（追加到当前镜像）」（备份模式 + 已存在的 `.wim/.esd` 才可点）；
    走现有 `RunBackup(append=true)`（同文件多时间点，文件级去重），完成后提示"现在是第 N 个时间点，合计 X GB"；
    **不设保留上限、不自动删**（删时间点=整包重写）。加密+增量本轮不做（`RunBackup` 直接拒绝 `--append --password`）。
  · **精度坑**：`CryptProgress` 的约定要与 `ProgressFn` 一致（都返回 true=取消）——第一版写反了，CLI 一跑就报"已取消"（实测抓出）。
  · **验证**：单测 `crypt_zje_roundtrip_meta_and_failclosed`（3MB+12345 字节跨块往返、错口令、篡改 1 字节 → 全部 fail-closed 且不留明文）；
    CLI 端到端：`backup --password` → `.zje`（5,244,845B，无明文残留）→ `images --password` 列出子镜像 / 错口令与无口令均拒绝 →
    `verify --password` 通过 → `extract --path \a.bin` 3,145,728B 逐字节一致 → `restore --engine block --password` `rc=4`、无口令 `rc=4`。
  · **2026-10-29 最终定稿：改为 dm-crypt（Linux 侧真解密，零额外空间）**。用户否掉了"暂存解密"（空间/时间翻倍）。
    - 容器改 v2：正文 = `aes-cbc-essiv:sha256` **512B 扇区**连续流（v1 只保留读取）。
    - 救援层：`unlock_image`（口令从 /dev/console 读）→ `openssl kdf` 派生 → 校验头 MAC →
      `losetup -r` + `dmsetup create ... crypt aes-cbc-essiv:sha256 <key> 0 <loop> 2048` → `IMG_FILE=/dev/mapper/zjenc`。
    - **QEMU 实测三个坑**：① `sector_size:4096`（含 `iv_large_sectors`）被 Debian 6.12 的 dm-crypt
      直接 `EINVAL` → 统一 512B 扇区；② 正文长度必须 `(plain+511)/512*512` 且不得越过 loop 末尾
      （多算 4KB 就 EINVAL）；③ Windows CBC 的 `cbIV` 必须显式 16（传 0 → `STATUS_INVALID_BUFFER_SIZE`）。
    - **验证**：guest 里 `openssl kdf` 复算的 PBKDF2 与 Windows 一致（头 MAC 相等），映射后
      `wimlib-imagex info /dev/mapper/zjenc` 返回 OK。initramfs +2.7MB（openssl/device-mapper）。
    - 「安装到启动」对加密镜像**重新放开**：菜单是用户开机时主动选的，可以当场输口令。
  · **2026-10-29 演练盘实测（QEMU，B 引擎全链）**：`ZJ_DRILL_ENC=<.zje>` 生成演练盘 →
    串口输入口令 → `password ok (header MAC verified)` → `dm-crypt` → `apply done` →
    `bootfix` → `boot region OK` → `RESTORE DONE` → 重启。
    **顺带修掉一个真问题**：`unlock_image` 原先只派生密钥就映射、**没验头 MAC**，口令错也会继续
    （最后只报 wimlib "Invalid magic characters"，用户根本不知道是密码错）。现在用
    `openssl kdf -keylen 64` 同时拿 encKey/macKey，先验头 MAC，错口令 → `STEP=enc-password` 明确提示。
  · **`2>/dev/null` 新增 9 处（A 类）**：`modprobe`（模块可能内建）、`dmsetup remove`/`losetup -d`
    （清理幂等）、`openssl ... -P`（失败另有 keylen 检查）、`dmsetup mknodes`（无 udev 时可能无输出）。
  · **GUI 实测补记（口令框）**：① 原生 EDIT 是 **首次获得焦点时才创建**（Duilib `CEditUI::DoEvent` 收到 `UIEVENT_SETFOCUS`/`BUTTONDOWN`/`MOUSEENTER` 才 `new CEditWnd`），
    光 `SetPos` 建不出来 → `CPasswordDlg::Ask` 里必须 `m_PaintManager.SetFocus(edit)` 触发；② 单行 EDIT **吃掉回车/ESC**（不往父窗口转发）→ 子类化 `SetWindowSubclass` 转发 `WM_KEYDOWN`；
    ③ 自动化脚本找原生 EDIT 要按类名 **`EditWnd`**（不是 `Edit`）；④ 实测通过：粘贴 `.zje` 路径 → 弹「加密镜像」口令框（● 掩码）→ 输入口令回车 → 第一步下拉出现 `1 - Backup`、主按钮转蓝。
  · **明文兜底**：`RunBackup` 加了 RAII `PlainGuard`（任何提前 return 都删 `<目标>.plain.tmp`）；GUI `CleanupIncompleteOutput` 的候选路径同时覆盖 `.plain.tmp{,.stage,.stage.tmp}`。
    实测：目标名被目录占用导致加密改名失败 → `rc=1` 且**没有留下明文**。
  · **2026-10-29 更正：引擎矩阵改为 A/B 双路（用户要求）**。原先 B 引擎被拒，理由是"救援层没有解密器"。
    落地方式 = **暂存阶段由 Windows 解密**：
    - `StageRestoreImpl` 在"镜像搬出目标分区"之后、写契约之前，把 `.zje` 解密到**同卷**临时明文
      （`~zjenc-<pid>-<tick>.tmp`），`image_path`/`image_rel_path`/`image_part_guid` 全部指向它，并写
      **`image_cleanup=1`**；救援层 `cleanup_after_success` 挂载该分区后 `rm -f` 掉（只在成功路径删，
      失败保留以便 P3 自动重试/排查）。
    - **空间预检**：同卷需要 ≈ 镜像大小 + 256MB；不够直接拒绝并给三条出路（清理 / 改 A 引擎 / 未加密镜像）。
    - **兜底**：契约成功后同时 `MoveFileExW(..., MOVEFILE_DELAY_UNTIL_REBOOT)` —— 用户没进救援菜单而是
      正常启动 Windows 时，这份明文会在那次开机被删掉（不会长期躺在盘上）。
    - **EFS/不可写回重解析点**：解密后立刻在明文上补做 `ScanImageForLinuxRestore`（原先对 `.zje` 必然
      失败，只能写 2=不可用让救援层兜底），命中即 `rc=4`、未动任何分区，并把 `reparse_scan/efs_scan=0` 写进契约。
    - **为什么不选"就地改字节解密"**：那会把加密镜像本身毁掉（改到一半断电 = 密文/明文交错的废文件），
      还原完还得整包重加密（再一遍全量 IO），且整个还原期间镜像以明文躺在盘上；收益（省一份临时空间）
      远小于风险。真正的"零额外空间 + 开机输入口令"只有 dm-crypt 路线（需要把容器改成 dm-crypt 能读的
      CBC-ESSIV 布局 + 给 initramfs 加 device-mapper/openssl，属独立一笔工作，见 docs/30 §11）。
  · **`2>/dev/null` 新增一条**：`rm -f "/tmp/zj_cl/$IMG_REL" 2>/dev/null`（A 类：文件已被前一轮清掉时
    rm 的报错没有信息量，成功/失败另有 `say`）。

### PIT-189

- PIT-189 **异机还原"105 个驱动一把梭"→ 2 个非签名启动驱动让整单失败 + 失败后先删 30GB 才报错**（`0.9.61`，2026-10-30，客户日志 `SysRecover-logs-20261029-234720`）
  · **现象**：30.4GB 镜像 + A 引擎 + 异机还原，23:24 开始解压（7m47s）→ DISM 注入 **105 个包**（4m53s）→
    `rc=50` → **23:45 才弹错误**（中间 8m37s 在删 `C:\~new`）。客户结论是"不如重装系统"。
  · **DISM 真因**（`dism-universal.log`）：`hidi2c.inf` / `netvwifibus.inf` 是"未签名的启动关键驱动"，
    DISM 默认拒绝（`0x80070032`，提示 `/forceunsigned`），其余 103 个都成功。它们**不是存储驱动**，
    对能否开机毫无影响。
  · **我们自己的四个问题**：
    ① 驱动文件夹是 `dism /export-driver` 的**全量 1.8GB** + Dell WinPE 包（网络/打印/蓝牙/显卡），
       DISM 一把梭 `/Recurse`，90% 的包对启动无用；
    ② 任何单包失败 = 整单失败（`rc != 0` 直接 return）；
    ③ 失败路径**先 `DeleteTreeRec(~new)` 再组装错误**（30GB 删除 ~8 分钟无响应），且重试还要重新解压；
    ④ 错误文案写"确认驱动与镜像**位数/签名**匹配"——用户因此以为是位数问题（实际是未签名项）。
    另：INF 递归计数有 `depth > 8` 上限，Dell Pack 最深 11 层 → 我们数 73、DISM 找到 105（潜在漏扫）。
  · **修复（本版）**：
    - 只注入**启动关键类**：`SCSIAdapter/HDC/System/DiskDrive/Volume/USB/HIDClass/Keyboard/Mouse/SDHost`
      （解析 INF `[Version] Class=`；其它类直接跳过）。
    - 跳过 `Provider=Microsoft`（镜像自带，且 DISM 会拒绝其中未签名项）与 **OS 不匹配目录**
      （Win10 镜像不灌 `Windows11-x64` 驱动；也不灌 Win7/8 驱动）。
    - **逐包注入**：失败只记日志并跳过（**不做 `/ForceUnsigned` 强装**——强行装未签名启动驱动会让 Secure Boot 起不来）；只有"有候选但一个都没成功"才失败。
    - 失败**保留 `~new`**：写完 `.zj-done`（可复用）+ 失败标记 `.zj-universal-failed`；重试只重注入驱动（几十秒），
      `zjswap.sh` 见到失败标记**拒绝交换**（双保险）。错误立即返回，不再先删 30GB。
    - GUI：勾了「异机还原」时「重启后还原」按钮**置灰**（红），并写明"只能选「还原后重启」，不需要 PE"。
    - 文案：CSM/GPT、UEFI/MBR 两条改为"进 BIOS 改哪一项 / 用对应模式的 PE 就地还原"。
    - INF 扫描深度上限 8 → 32。
  · **新增「导出本机启动驱动」**（GUI「文件>系统」菜单 + CLI `export-drivers [--dest]`）：
    `pnputil /enum-drivers` 按 **Class GUID** 过滤（GUID 与系统语言无关，`pnputil` 的标签会本地化），
    再逐个 `pnputil /export-driver`。实测本机 28 个 oem 包 → 只导 12 个、**6.9MB**（对比全量 1.8GB）。
  · **关于"提示进 PE"**：客户遇到的是"异机还原 + 重启后还原"的组合（B 引擎在 Linux 跑，没有 DISM），
    **不是**镜像与固件相关；正确做法就是选「还原后重启」。真正的 PE 限制只有"固件模式 ≠ 目标盘分区风格
    且要还原正在运行的系统盘"（CSM+GPT / UEFI+MBR），因为重启后回不到救援层。

  · **2026-10-30 补记（导出即自动生效，文案纠正）**：客户质疑导出弹窗写
    "异机还原时把「驱动目录」指向它即可" —— 其实 GUI 早就是自动的
    （`StartRestoreAsync` 固定取 `ExeDir()\Drivers`，`ScanInfs` 递归 32 层，
    导出的 `Drivers\boot-<时间>\` 子目录会被自动扫到），只是文案说成了要手动指定。
    现在：① 弹窗改为"只含存储/输入类驱动，异机还原时将自动调用此驱动。"；
    ② GUI 在还原时写日志 `universal: auto driver dir=... inf=N`（没有 Drivers 目录则记
    `in-box storage drivers only`）；③ CLI 的 `--universal` 不给 `--drivers` 时
    也自动用程序目录的 `Drivers\`，与 GUI 同口径。

### PIT-190

- PIT-190 **「删除旧系统」点不动 + 重启后 ~old 还在且没权限**：进度窗 TOPMOST 挡住确认框、X 被吞、直接夺权在 TrustedInstaller ACL 上无效（`0.9.63`，2026-10-30，客户日志 `SysRecover-logs-20261030-105636`）
  · **现象**：客户选「删除旧系统后自删」，然后"点不动"；重启后 `C:\~old` 还在，资源管理器删不掉（无权限）。
  · **日志证据**：
    - 删除本身一直在跑（10:53:35–10:56:37，`ok≈2.7 万 / fail=230 / taken=0`），**进程没卡死**；
    - 10:56:34 主界面还能响应「日志」点击 → 说明删除进度窗是 `WS_EX_TOPMOST`，而**主窗没被禁用**；
      用户点「日志」后弹出的确认框（完成/重启安全模式）出现在进度窗**后面** → 看起来"按钮点不动"；
    - `WM_CLOSE` 当时是 `return 0`（防中止）→ 点 X 完全没反应；
    - `takeown` 走了 10 次、`taken=0`：`SetNamedSecurityInfoW` 那套**直接夺权在部分 .NET/TrustedInstaller
      ACL 上不生效**，230 个文件 `err=5`，目录连锁 `err=145`；
    - 现场还有 **2 个 explorer dump** + 事件日志 **NTFS Event 55（MFT 损坏）**：删除把磁盘打满，
      桌面都卡；这块盘本身可能有问题。
  · **根因三条**：① 交互/反馈设计（顶层窗挡对话框 + X 无响应 + 没有进度条）；② 权限兜底不够；
    ③ 失败后 `.zj-keepold` 已被删（首轮就删了），重启后无法确认"用户是想删的"。
  · **修复**：
    - 进度窗加**进度条**（跑马灯，无 v6 控件时退回定时器）+ 已删/失败/秒数；`WM_CLOSE` 与
      「**在后台继续**」按钮改为**隐藏窗口、删除继续**，并**恢复主窗可用**；
    - 删除期间**禁用主窗**（防再点出被挡住的对话框）；
    - 删除线程切 **`THREAD_MODE_BACKGROUND_BEGIN`**（后台 I/O 优先级）——磁盘忙时前台优先，
      代价是删得稍慢，但不再把 explorer 拖死；
    - 首轮失败 → `takeown /f <dir> /r /d y /a` + `icacls <dir> /grant *S-1-5-32-544:F /t /c /q`
      批量夺权后**再删一次**（实测：TrustedInstaller 拥有 + 只剩 SYSTEM 权限的文件，直接删 `err=5`，
      批量夺权后可删）；
    - 仍失败 → 生成 `C:\Windows\Temp\zj-cleanup-old.cmd` + **`schtasks /sc onstart /ru SYSTEM`**
      计划任务，**下次开机由 SYSTEM 删除**（无句柄占用、SYSTEM 有完全控制），成功后任务自删；
      删除期间保留 `.zj-keepold`（成功后再删），失败时 UI 明确写"已登记下次开机自动清除"。
  · **回答客户的三问**：① 文件太多显示不过来**不是**瓶颈（200 个才刷一次）；② 大量磁盘操作**确实**
    会拖死桌面（explorer dump 为证）；③ 限制 CPU **基本无用**（删除是 I/O 型），**后台 I/O 优先级有用**；
    ④ "做成插件后台慢慢删"不如**开机 SYSTEM 任务**（零锁、零权限问题、无窗口）。
  · **用户侧手动清理**（管理员 CMD）：`takeown /f C:\~old /r /d y` →
    `icacls C:\~old /grant *S-1-5-32-544:F /t /c` → `rd /s /q C:\~old`；
    仍不行就重启后再删（或先 `chkdsk C: /f`——现场有 MFT 损坏事件）。
  · **2026-10-30 追加（用户 4 点反馈）**：
    - 进度回调从"每 200 个文件"改为**每 0.5 秒**（保留 200 个作为快速删除下限）——
      删到一半界面不动最容易被当成卡死；
    - 超过 5 秒后进度窗追加一行："文件较多，可能要几分钟：可点「在后台继续」去做别的，删完会提示"；
    - **防叠删**：`DeleteSwapLeftoverAsync` 先占 `g_busy` 再起线程，同一盘已有删除在跑就直接跳过
      （否则每次开机/重复点击都会叠一个线程，互相抢句柄 + 重复 takeown）；
    - **批量夺权限流**：剩余失败 > 3000 项时不再跑 `takeown /r`（会把剩余树再走一遍，
      还可能跟 junction 跑），直接交给开机 SYSTEM 任务。

### PIT-191

- PIT-191 **ESP 空间检查把"全量"当"净增量"：明明只是覆盖同名文件，却报"需要 29.5MB、可用 29.4MB"**（`0.9.63`，2026-10-30）
  · 客户在导出驱动后立刻「还原后重启」，弹："安装UEFI引导层失败 … ESP space: need 29.5MB, free 29.4MB"。
    看起来只差 **0.1MB**，实际比较条件是 `free < need + 16MB(bcdboot 余量)` —— 差的是 16MB，
    而且 ESP 上**已经有上一版同尺寸的载荷**，覆盖写只需"增量"空间。
  · 根因：`need += FileSize(src)` 按**全量**算，没有减去已存在的同名文件大小。
  · 修复：`need += max(0, srcSize - dstSize)`（覆盖是原地替换）；失败文案改为
    "还需 X MB，另需预留 16MB 给 bcdboot，可用 Y MB"，不再出现"看着只差 0.1MB"的误导。
  · 与「导出本机驱动」无关：导出写的是程序目录（C:），不占 ESP；只是时间上挨着，属误判。

### PIT-192

- PIT-192 **异机还原误报「镜像内缺少 ntoskrnl.exe」= 新系统里 `%TEMP%` 整个目录不存在（wimlib rc=48）**（`0.9.66`，2026-10-30，客户日志 `SysRecover-logs-20261030-191244`）
  · **现象**：23 号台式机的镜像异机还原到笔记本，勾「异机还原」后点开始 → `暂存失败：异机还原：无法读取镜像内的系统版本（镜像内缺少 ntoskrnl.exe）`；
    换/加笔记本驱动包后又能过 → 用户以为"驱动包不够"。
  · **证据链**：`wimlib-errors.log` 三行
    `[ERROR] Can't open or create directory "C:\Users\Administrator\AppData\Local\Temp\zjuni-1856" (status=c000003a)`
    —— `c000003a` = **STATUS_OBJECT_PATH_NOT_FOUND**，即**父目录不存在**；主日志对应
    `universal: image metadata ntoskrnl=yes winload=yes` → `extract version probes rc=48`
    （wimlib 48 = `WIMLIB_ERR_OPENDIR`）→ 我们却报了「镜像内缺少 ntoskrnl.exe」。
  · **根因**：`ProbeImageOs` 用 `GetTempPathW` 拿 `%TEMP%`，而它**只回显环境变量、不建目录**；
    这台机器刚用 17763 镜像还原过（18:19 暂存 → 18:23 新系统首启），新系统里
    `C:\Users\Administrator\AppData\Local\Temp` **整个目录都不存在**，
    单层 `CreateDirectoryW` 建不出来 → wimlib 打不开目标目录 → 探测失败 → 误报镜像坏。
    18:33 再试就"成功"了：那时 TEMP 已被别的程序建出来 —— 与"加驱动包"无关，纯巧合。
  · **修复**：
    - 新增 `common/relocate.{h,cpp}::EnsureTempDir()`：**递归**建 TEMP；建不出来（只读/权限）
      就把 `TEMP`/`TMP` 环境变量改指 `<Windows>\Temp`。GUI/CLI 启动时各调一次（幂等）；
    - `ProbeImageOs`：递归建探测目录 + 三级回退（`%TEMP%` → `<Windows>\Temp` →
      `<日志根>\probe`），每级失败都记 `universal: probe temp unusable: ... err=N`；
    - **错误文案分清两种**：元数据里有 / wimlib 报错 → 「无法从镜像解出版本探测文件（临时目录不可用或磁盘已满，wimlib rc=N）」；
      只有元数据确实没有才说「镜像内缺少 ntoskrnl.exe」。
  · **同时修掉的两个连带问题**（同一次现场日志暴露）：
    - `Drivers\boot-<时间>\` 每导出一次就多一份（客户导了 4 次），注入时同一驱动被 DISM
      注入 4 次（日志：`added iaAHCIC.inf` × 4、`kept=33` 实际只有 11 个不同的驱动）→
      现在导出成功后**只保留最新一份**（删同级旧 `boot-*`，记 `pruned N old export folder(s)`）；
      注入前再按 INF 文件名去一次重（同名留修改时间最新的，记 `dedupe inf by name kept=N dropped=M`）。
      `Drivers\` 下用户自己放的其它目录（如笔记本驱动包）**不动**。
    - 导出/探测日志里的中文路径原先用 `std::string(w.begin(), w.end())` 转，落盘是
      `E:\]l؟\Drivers\...` 这种乱码 → 统一改 UTF-8。

### PIT-193

- PIT-193 **「暂存失败：目标系统缺少引导模板（missing \Windows\System32\Config\BCD-Template）」= 32 位进程被 WOW64 重定向 + 检查 fail-closed**（`0.9.67`，2026-10-31，维护者本机实测）
  · **现象**：本机重装系统，选「还原后重启」→ 提取 6GB 到 `C:\~new` 全部成功、契约已写、ESP 已挂载，
    最后一步报"目标系统缺少引导模板（missing \Windows\System32\Config\BCD-Template）：bcdboot 无法生成引导，
    请换用完整系统镜像"，然后**整单回滚**（契约删掉、`~new` 留着）。用户看文件明明存在（28672 字节）。
  · **两个独立根因**：
    ① 跑的是 **`SysRecoverUI_x86.exe`**（显式 32 位入口）。`ReexecX64IfNeeded()` 只找
       `<dir>\x64\<同名>`，而 x64 目录里没有带 `_x86` 后缀的文件 → 自举失败 → 一直以 32 位跑。
       32 位进程访问 `C:\Windows\System32\...` 会被 **WOW64 重定向**到 `SysWOW64`，而
       `SysWOW64\Config\BCD-Template` **不存在** → 误判"缺模板"。
       （实测：`SysWOW64\cmd /c if exist C:\Windows\System32\Config\BCD-Template` → MISSING；
       `C:\Windows\Sysnative\Config\BCD-Template` → OK。）
    ② 即使真的缺模板也**不该判死**：我们有随包资产 `bootfiles\bcd\uefi.bcd` +
       `EnsureUefiBootFiles()`（复制 `\Windows\Boot\EFI`、补 bootmgfw、按真实分区 GUID 打补丁），
       整条链**不需要 bcdboot、也不需要 BCD-Template**。就地还原路径早就这么兜底了，
       只有**暂存路径**还在 fail-closed（漏改）。
  · **修复**：
    - `selfarch.cpp`：`_x86` 后缀入口的自举目标先去掉 `_x86` 再找 `x64\<名>`（`SysRecoverUI_x86.exe`
      → `x64\SysRecoverUI.exe`）。实测 `SysRecover_x86.exe diag` → `proc=x64 wow64=no`。
    - `uefi.cpp::TargetBcdTemplateOk`：32 位进程再查一次 `%windir%\Sysnative\Config\BCD-Template`
      （绕过 WOW64 重定向），命中即记 `BCD-Template found via Sysnative`。
    - `ops.cpp`（暂存 UEFI 分支）：模板/空间预检从"直接失败并回滚"降级为**只决定走不走 bcdboot**，
      不满足就用自足路径；新增纯函数 `bootself::PlanUefiBcd()`（keep / bcdboot / self-contained）+ 单测。
  · **教训**：凡是读 `<系统盘>:\Windows\System32\...` 的判断，32 位进程都必须走 Sysnative
    （与 PIT-082 的 bcdedit 同类坑）；凡是"我们有自足方案"的预检，都不该做成 fail-closed。

### PIT-194

- PIT-194 **暂存成功、重启却直接进了旧系统（UEFI 单次启动 BootNext 未生效；临时 Boot#### 事后消失）**（`0.9.68` 加复核日志，2026-10-31，维护者本机）
  · **现场**：0.9.67 暂存 UEFI 还原，日志完整：`ESP BCD already valid -> skip bcdboot`、
    `firmware boot entry Boot0003 installed (desc=SysRecover Temporary Restore, optional=92 chars)`、
    `restore staged, reboot to execute` → `ExitWindowsEx ok`。
    重启后（Kernel-General：15:57:39 关机 / 15:57:56 开机，**只有一次启动**）直接进了旧系统。
  · **证据（排除法）**：
    - 救援层**根本没跑**：ESP `\EFI\ZJRESTORE\` 里只有内核/initramfs（无 `ZJRESTORE-last.log` 等），
      目标盘根无 `zjrestore-boot.log`，`C:\~new`+`.zj-done`+契约 `_zjresy311557.log` 全部原样。
    - 事后读固件变量：`BootNext` **不存在**，`BootOrder` 只剩 3 项（Windows/SysRecover/USB），
      临时项 `Boot0003` **已消失**；常驻项 `Boot0000`（desc=SysRecover，optional 里 initrd 正确）还在。
    - 现场实测 NVRAM **可正常持久化**（写 `Boot00FE` 复制项、追加到 BootOrder 末尾，60 秒内未被回滚，
      清理正常）→ 不是"NVRAM 写不进去"。
    - 火绒（HipsDaemon）在跑；日志里**没有**它拦截的直接证据。
  · **结论**：写入那一刻是真的（读回校验通过），但**重启后固件没按 BootNext 走**，且临时项被清掉；
    具体是固件忽略 BootNext / 固件对"加载失败的项"做清理 / 安全软件在重启前后回滚，三选一，
    当前证据不足以判定（下次带复核日志即可分辨）。
  · **本次改动**：
    - `SetUefiBootNext` 的返回消息改用**独立日志串**并立刻落盘（以前写进 `blog`，而 `blog` 在
      装引导项时就打过了 → `BootNext = Boot0003 (verified)` 这行**从来没进过日志**，是最大的排错盲区）；
    - 新增 `UefiTempBootStateOk()`：**重启前复核**「Boot#### 仍在 BootOrder + BootNext 指向它」，
      不通过就重写一次并留 `boot state ...` 证据；仍不通过则记 ERROR 并提示"重启若回桌面请选开机菜单
      里的 SysRecover / 退出防护后重试"。
  · **现场可用的绕过**：开机菜单（F11/F12/ESC，视主板）里选常驻项 **SysRecover** ——
    它指向同一套内核/initramfs，会执行 C: 上**已经暂存好**的任务，等同完成本次还原。

### PIT-195

- PIT-195 **重启交接全面收口：BootNext 不可靠 → 改用「BootOrder 置顶 + 独立临时内核 + 自失效」；复核不过禁止重启**（`0.9.69`，2026-10-31，专家评审见 docs/32）
  · **背景**：PIT-194 那台机器写 BootNext 明明"读回校验通过"，重启却直接进旧系统；同类问题在客户机上还会以
    "临时项消失 / 多项都指向同一 ESP / 重启后无任何提示"等形式出现。补丁式修不动了，按 docs/32 收口。
  · **收敛后的协议（UEFI，SB off）**：
    1. 载荷部署时多放一份**独立临时内核** `vmlinuz-zjtemp.efi`（与常驻菜单的 `vmlinuz-zjrestore.efi` 解耦）；
    2. 临时 `Boot####` 指向这份临时内核；
    3. **BootOrder 置顶**（`bootorder.bak` 留原序）+ 仍设 BootNext（能认就走快路）；
    4. 复核 = 条目在 + **临时内核在** + (BootNext 指向 或 BootOrder[0] 是它)；
    5. 复核失败 → 重写一次；**仍失败 → 取消本次、不重启**（目标零改动），文案直接给两条出路：
       退出安全软件后重试 / 用 WinPE 就地还原；
    6. 救援层**一启动就删临时内核** → 无论成功失败断电，置顶项都不会再把机器拉回救援（防死循环）；
    7. 新系统首启清理助手删临时项 + ESP 载荷；`CleanupStaleUefiEntries()` 清"临时内核已不存在"的旧项。
  · **顺带修掉的两个硬伤**：
    - `ReadVar(BootOrder)` 失败被当空表 → 会把 BootOrder 覆盖成只剩我们一项（其它系统从启动菜单消失，可能"无法开机"）→ 现在读失败即 fail-closed；
    - `--no-repair-boot` + 暂存（非就地）= 不装任何引导却返回成功并提示"重启后执行"（100% 进旧系统）→ 现在前置拒绝（退出码 2）。
  · **回归判据**（docs/32 §5）：删掉 `vmlinuz-zjtemp.efi` 后重启必须回落 Windows；清掉 BootNext 后仍能进救援；
    日志必须出现 `staged entry BootXXXX promoted to BootOrder[0]` 与 `boot state verified: ...`。
