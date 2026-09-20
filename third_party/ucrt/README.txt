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

怎么取（二选一）
----------------
① 装了 Visual Studio / Windows SDK 的机器：
   把 `C:\Program Files (x86)\Windows Kits\10\Redist\ucrt\DLLs\x64\` 下的
   **全部 .dll**（`ucrtbase.dll` + 约 40 个 `api-ms-win-crt-*.dll` / `api-ms-win-*.dll`）
   拷到本目录的 `x64/` 子目录。

② 没装 SDK：官方下载页 id=48234（Windows 10 Universal C Runtime）的 `WindowsUCRT.zip`，
   安装 `Windows6.1-KB2999226-x64.msu` 后，从系统里取（或从 MSI 安装日志定位）。
   ⚠️ 直接用 7-Zip 解 MSU 只能拿到 CBS 的 **PA30 容器**（magic `PA30`），解不出 DLL。

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
