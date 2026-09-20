SysRecover —— Windows 7 需要的 Universal CRT（UCRT）运行库
================================================================

为什么需要
----------
本产品的 exe 与随包的 libwim-15.dll 都依赖 **UCRT**（`ucrtbase.dll` +
`api-ms-win-crt-*.dll`）。Win10/11 内置，**Windows 7 没有** → 不随包就会报"缺少 DLL"。

微软官方文档明确支持"随应用本地部署"：
  "Local deployment of the Universal CRT is supported... The DLLs for local deployment
   are included as part of the Windows SDK, in the '
   Windows Kits\10\Redist\ucrt\DLLs' subdirectory, by computer architecture.
   It's highly recommended that you include all of the DLLs when you deploy locally."
  "Versions of Windows before Windows 8: ... Place the Universal CRT files in the
   same directory as the app."

所以：**把这些 DLL 放到本目录（x64/），`make package` 会自动拷到 dist/ 与 exe 同目录。**

怎么取（三选一，推荐 ① 或 ②）
------------------------------
① **最快（已实测）**：在一台 Win7 x64 上装
   `Windows6.1-KB2999226-x64.msu`（来自官方 `WindowsUCRT.zip`，下载页 id=48234）
   或 **VC++ 2015-2022 x64 运行库**（https://aka.ms/vs/17/release/vc_redist.x64.exe），
   装完 C:\Windows\System32 里就有真文件了，把下面两类全拷到本目录 `x64/`：
       C:\Windows\System32\ucrtbase.dll
       C:\Windows\System32\api-ms-win-crt-*.dll
   （内容与 SDK 里的 Redist 一致）

② 装了 Visual Studio / Windows SDK 的机器：
   把 `C:\Program Files (x86)\Windows Kits\10\Redist\ucrt\DLLs\x64\` 下的
   **全部 .dll** 拷到本目录的 `x64/` 子目录（微软建议"整套带上"）。

③ 官方下载页 id=48234 的 `WindowsUCRT.zip`（含各系统 MSU）。
   ⚠️ 注意：直接用 7-Zip 解 MSU 只能拿到 CBS 的 **PA30 容器**（magic `PA30`），解不出 DLL；
   必须先**安装**该 MSU，再从 System32 取（即方法 ①）。

授权
----
UCRT 属可再发行组件（微软官方许可允许随应用分发）；`WindowsUCRT.zip` 内的
`sdk_license.rtf` 与 `Windows UCRT ReadMe.rtf` 是随附许可文本，可一并保留。

体积参考
--------
`old` 版 UCRT DLL 约 **1.5 MB**（40 个文件左右）。

当前状态
--------
⬜ 尚未放入 —— 放入后 `make package` 会把它们拷进 `dist/`，Win7 即可零安装运行。
（未放入时打包照常，只是 Win7 需要用户自行安装 VC++ 运行库。）
