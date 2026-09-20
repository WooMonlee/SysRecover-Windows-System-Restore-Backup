# UI 外观设计（对齐老项目 WPF 1:1）

> 来源：`D:\Prog\_Project\SysRestore\src\ZjRestore.Gui\MainWindow.xaml`（870×410，无边框圆角 `CornerRadius 12`，背景 `#eef1f6`）+ `MainWindow.xaml.cs`（450 行）+ `PartitionRow.cs`。
> 结论：老外观无问题，C++ 版照抄布局与配色，只换实现（WPF XAML → Duilib XML）。骨架见 `../skin/main.xml`。
> 状态：✅ 设计定稿 2026-09-05 / ⏳ Duilib 联调待验证。

## 1. 总布局（上→下，与老项目一致）

```
┌─ 顶栏 (#f7f9fc) ─────────────────────────────┐
│ (Tab)镜像恢复为系统 | 系统备份为镜像  知鉴一键还原 v0.1   — □ ✕ │
├─ 第一步（白卡 #ffffff）──────────────────────┤
│ [第一步] [浏览系统镜像文件] [镜像路径输入框____] │
│          镜像说明： [子镜像下拉] 备注文字        │
├─ 第二步（蓝卡 #f0f5ff）──────────────────────┤
│ [第二步] 系统安装位置 + hint                   │
│  盘符·系统类型 │ 磁盘 │ 分区 │ 容量·可用        │
│  [分区下拉（四列行 + 用量进度条）___________]   │
├─ 第三步行 ───────────────────────────────────┤
│ [第三步] [ 开始恢复系统 ]   静默模式☑ 保存格式▼ [清除单次引导] [生成启动菜单] │
├─ 状态栏 (#fafbfe) ───────────────────────────┤
│ 执行进度 [████████████░░░░░░░░]                 │
└──────────────────────────────────────────────┘
```

## 2. 控件映射表（WPF → Duilib，id 同名）

| 老 id（WPF） | 类型 | 新 id（Duilib） | Duilib 控件（经典库标签名） | 备注 |
|---|---|---|---|---|
| ModeRestore / ModeBackup | RadioButton（ModeTab 样式） | 同名 | `<TabOption>`（自绘 `CTabOptionUI`） | 选中蓝字 + 底部 4px 圆角下划线；切换触发 `OnModeChanged` |
| ImagePath | TextBox（RoundTextBox） | 同名 | `<SkinEdit>`（自绘 `CSkinEditUI`） | 圆角 8，边框 `#c9d6f2` |
| (浏览) | Button（FlatBtn） | `BrowseBtn` | `<SkinButton>` | 白底圆角 7，hover 边框 `#2b5ce0` |
| NoteLabelRestore/Backup、NoteText | TextBlock | 同名 | `<SkinLabel>` | 次要文字 `#5f6c80` |
| ImageIndexBox | ComboBox（`Index - Name`） | 同名 | `<Combo>` | **无外框**；下方单独一条 `<Control>` 画 1px 横线（详见 §6.6）。备份模式隐藏 |
| NoteInput | TextBox（UnderlineTextBox） | 同名 | `<SkinEdit underline="true">` | **只画最下方一根横线**（对齐老项目 `UnderlineTextBox`）。备份模式显示 |
| PartitionBox | ComboBox（四列 DataTemplate） | 同名 | `<Combo>` + `<PartItem>`（自绘 `CPartItemUI`，重写 `DoPaint`/`DrawItemText`） | 见 §3 |
| PartLoadingText | TextBlock | 同名 | `<SkinLabel>` | "正在读取分区信息……"，加载完隐藏 |
| MainAction | Button（PrimaryBtn） | 同名 | `<SkinButton>` | 16px Bold；`#8ca0de`，禁用态 `#e2e6ee` |
| Silent | CheckBox（默认勾选） | 同名 | `<GlyphCheck>`（自绘 `CGlyphCheckBoxUI`） | 静默模式 |
| FormatBox | ComboBox（.esd/.wim） | 同名 | `<Combo>` + `<TextItem>`（自绘 `CTextItemUI`） | 备份模式显示 |
| FormatLabel | TextBlock | 同名 | `<SkinLabel>` | "保存格式"，备份模式显示 |
| (清除单次引导/生成启动菜单) | Button（FlatBtn） | `RepairBootBtn` / `BootMenuBtn` | `<SkinButton>` | 还原模式显示 |
| StatusText | TextBlock | 同名 | `<SkinLabel>` | "执行进度" |
| Progress | ProgressBar | 同名 | `<RoundProgress>`（自绘 `CRoundProgressUI`） | 前景 `#6a5ce0`，底 `#e8edf5` |
| — □ ✕ | Button（WinBtn） | `MinBtn/MaxBtn/CloseBtn` | `<WinBtn>`（自绘 `CWindowBtnUI`） | 关闭 hover `#e81123` 白字 |
| 标题 `知鉴一键还原 v0.1` | TextBlock ×2 | — | `<TitleLabel>`（自绘 `CTitleLabelUI`） | 20px Bold + 同行小字副标题 |
| 第一步 / 第二步 / 第三步 | Border（橙底圆角） | `Step1Badge` / `Step2Badge` / `Step3Badge` | `<SkinLabel>` | 橙底 `#e08a2e`，`borderround="8,8"` |

> **关键：自绘控件必须用「新标签名」**（`TabOption`/`GlyphCheck`/`RoundProgress`/`PartItem`/`TextItem`/`TitleLabel`/`SkinLabel`/`SkinButton`/`SkinEdit`/`WinBtn`）。内建名（`Button`/`Edit`/`Option`/`Combo`/`Label`/`Progress`…）在 `UIDlgBuilder.cpp:292-352` 就被拦截直接 `new` 出库内类，**不会回调 `WindowImplBase::CreateControl()`**，自绘类永远拿不到创建机会。注入点在 `CMainForm::CreateControl` → `ui_skin::CreateSkinControl`。

## 3. 分区下拉行（四列，对齐 PartitionRow.cs）

列宽（与老项目一致）：180 / 200 / 80 / 240。

| 列 | 内容 | 来源（PartitionRow.cs） |
|---|---|---|
| 0 盘符·系统类型 | `DriveLetter` 加粗 + `PartitionTableType`(MBR/GPT) + `DisplayType`（颜色 `TypeBrush`） | `DriveLetter`（空→"—"）、`PartitionTableType`（GPT/MBR 二值）、`DisplayType` = `IsSystem?"当前系统":Role` + 文件系统；颜色：系统绿 `#1f9d57` / ESP·恢复红 `#d64545` / BitLocker 棕 `#8a6a1f` / MSR·扩展灰 `#98a2b3` / 普通蓝 `#2b5ce0` |
| 1 磁盘 | `DiskModel`（小字灰） + `DiskSub`（卷标，无→"—"） | 同名 |
| 2 分区 | `PartNo` = `DiskNumber/PartitionNumber` | 同名 |
| 3 容量·可用 | 用量小进度条（`UsedRatio`）+ `CapacityText`（`已用 / 总 GB`）+ `FreeText`（`可用 x GB` / "系统保留"） | `CapacityText/FreeText/UsedRatio`，<1GB 显示 MB |

过滤规则（照搬 `LoadDisksAsync`）：隐藏移动盘、ESP、MSR、扩展分区；只留 ≥2GB；默认选中系统分区。

## 4. 模式切换行为（照搬 OnModeChanged）

- 还原模式：标题"镜像恢复为系统"，主按钮"开始恢复系统"，显示镜像下拉+说明；隐藏备注输入与格式选择。
- 备份模式：标题"系统备份为镜像"，主按钮"开始备份系统"，显示备注输入+格式下拉；隐藏子镜像下拉。
- 命令行自动模式（`--auto-backup/--auto-restore` + `--source/--dest/--image/--disk/--part`）：Phase 5 再移植，CLI 先行。

## 5. 配色 / 字体 token（勿自创）

`bg #eef1f6 / card #ffffff / blue-card #f0f5ff / topbar #f7f9fc / statusbar #fafbfe / text #1b2432 / secondary #5f6c80 / border #dfe5ee / edit-border #c9d6f2 / accent #2b5ce0 / primary #8ca0de / primary-hover #7a8fd1 / disabled #e2e6ee / step-orange #e08a2e / progress #6a5ce0 / close-hover #e81123`。

字号（对 `界面1.png` 实测 bbox 反推，勿自创）：正文/表头/标签 **13px**、输入框与次要按钮 **14px**、主按钮 `开始恢复系统` **16px Bold**、标题 **20px Bold**（`v0.1` 同行小字）、窗口正文基准 14px。中文字体名 `Microsoft YaHei UI`（老图 WPF 即此族；宽度实测与老图一致：主按钮文字 bbox 95px vs 96px、分区行 216px vs 216px）。

## 6. Duilib 落地注意事项

1. 进度/分区加载在工作线程，禁直接碰控件，必须 `::PostMessage` 到 UI 线程（100ms 节流）。
2. `skin.xml` + 本文件进 RC 资源或随包 zip，禁止依赖外部散文件路径。
3. DPI：Per-Monitor V2；100%/150%/200% 三档截图验收。
4. 无边框拖动：顶栏处理 `WM_NCHITTEST`/`HTCAPTION`（对齐老项目 `OnTitleBarMouseDown`）。落到 Duilib 就是一条 `<Window caption="0,0,0,36">`：**第 3/4 个值是距右/下边缘的偏移量，不是右/下坐标**，写成 `0,0,870,36` 会整窗拖不动（详见 AGENTS.md PIT-015）。
5. 最小化/最大化/关闭：自绘按钮，关闭 hover 红底白字。顶栏的按钮与模式 Tab 保持 `HTCLIENT`，只有空白/标题区域算作可拖动区。
6. **经典 Duilib 属性语义（与 nim_duilib / WPF 都不同，踩坑见 AGENTS.md PIT-016）**：
   - 控件级圆角只能用 `borderround="cx,cy"`，且 `cx` 是**椭圆直径**（半径 = `cx/2`，要 7px 圆角就写 `14,14`）；`roundcorner` **只认 `<Window>`**。
   - `pos="left,top,right,bottom"` 后两个是**右下角坐标**（不是宽高）。`float="true"` 的定位原点是父容器 `m_rcItem`：本皮肤嵌套 `Window → 根 VerticalLayout(inset 1,1,1,1) → Panel(bordersize 1, inset 1,1,1,1) → Content` 后 **Content.m_rcItem = (2,2,868,408)**，故 XML 里的坐标 +2 才是屏幕坐标（`main.xml` 内所有 pos 均已按此写）。
   - `inset` 是**给子元素的内边距**，不是外边距；无 `height` 的子控件会被父布局拉伸填满。
   - `font="N"` 必须先在 `<Window>` 下用 `<Font id="N" name=... size=.../>` 声明，否则回退默认字体。自绘控件不走这套，用 `fontsize`/`bold` 属性。
   - **1px 描边不能走 GDI+ 抗锯齿**：`SmoothingModeAntiAlias` 会把 1px 线摊成 2px 半透明（实测 `#c9d6f2` 被渲染成 `#e4eaf8`，正是 50% 混白）。必须走 GDI `CreatePen(PS_INSIDEFRAME)` + `HOLLOW_BRUSH` + `::RoundRect`（与 Duilib 基类 `CRenderEngine::DrawRoundRect` 同法），见 `ui_skin.cpp::StrokeRound`。
   - 文字抗锯齿用 **`CLEARTYPE_QUALITY`**：WPF 老图笔画覆盖率高于 GDI 灰度 AA（表头墨迹 老 1343px vs 新 1294px；改用灰度 `ANTIALIASED_QUALITY` 只有 ~82% 覆盖率，观感"发虚"）。
   - 无图回退缺失：库内 `Button/Option/CheckBox/Combo/Progress` 没有图片就**什么都不画**；`CComboUI` 收起态走 `PaintText`，条目类必须重写 `DoPaint`/`DrawItemText`（`CPartItemUI`/`CTextItemUI` 已覆盖）。
7. **「外框不可见，只留一根横线」的实现**（老项目 `UnderlineTextBox` 的等价物）：
   - `SkinEdit` 加 `underline="true"` → `CSkinEditUI::DoPaint` 只画 `rc.bottom-1` 那一条（不画背景/圆角框），线色取 `bordercolor`（缺省 `#c9d6f2`）。
   - `Combo`（`ImageIndexBox`）去掉 `bkcolor`/`bordersize`/`bordercolor`（`CComboUI` 没重写 `DoPaint`，继承 `CControlUI::DoPaint`，无颜色即完全透明），在其底边单独放 `<Control pos="...,135,...,136" bkcolor="#FFC9D6F2"/>` 当横线。
   - 老图实测：`镜像说明：` 文字在 y120..131，其右侧**没有任何输入框**，横线是新设计，须收在第一步卡片（底边 y142）内 → 底线定在 y137。
   - 横线长度与上方输入框**等宽**（x251..836，实测 `界面1.png` 备份模式的"备份备注"行就是 586px），两个模式保持一致。
8. **两个模式（还原 / 备份）的几何**：第一/二步与第三步主按钮**完全一致**（切换时不跳），差异只在第三步右侧 —— 备份模式多一个"保存格式"下拉（标签 x343..398 + 框 x404..585），静默模式相应左移到 x252。切换由 `CMainForm::ApplyModeUi()` 用 `SetPos()` 重设这三个控件，**两个分支都要写**（切回时要复位），只改文字和可见性不够。完整对照表与老图原始实测值见 `AGENTS.md §15`。
9. 同组 Option 的 `SELECTCHANGED` 在**取消选中**时也会触发，按 `GetName()` 直接切界面会把内容刷回上一个模式（Tab 切了、正文没变）→ 只认 `IsSelected()==true` 的那一次，详见 `AGENTS.md PIT-017`。
