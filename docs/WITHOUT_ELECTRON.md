# 不用 Electron 的独立窗口方案

Electron 安装会下载较大的运行时，在国内网络下容易**长时间卡住**。以下方案**都不需要安装 Electron**，效果上仍可接近「单独一个窗口看远程应用」。

## 方案 A：系统 Edge / Chrome 应用窗口（推荐，零依赖）

用已安装的 **Microsoft Edge** 或 **Google Chrome** 的 **`--app=`** 模式：无标签栏、无地址栏，像独立客户端。

仓库提供脚本（见 **`scripts/open-remote-app.ps1`**）：

```powershell
cd 仓库根目录
.\scripts\open-remote-app.ps1
# 指定信令：
.\scripts\open-remote-app.ps1 -Signaling "ws://192.168.1.10:9090/"
# 全屏、尽量无标题栏感（Kiosk，退出多用 Alt+F4）：
.\scripts\open-remote-app.ps1 -WindowMode Kiosk
# 应用窗口 + 启动即全屏（仍可能保留一条顶栏，依浏览器版本）：
.\scripts\open-remote-app.ps1 -WindowMode Fullscreen
```

**关于标题栏**：纯网页**无法**去掉操作系统给浏览器画的标题栏。`--app` 已去掉标签栏/地址栏；在 Windows 上**完全像无边框客户端**时，优先用 **`-WindowMode Kiosk`**，或使用下方 **WebView2 启动器**（WinForms 可设 `FormBorderStyle=None`）。

也可手动（把路径改成你的 `index.html` 实际路径）：

```powershell
& "$env:ProgramFiles (x86)\Microsoft\Edge\Application\msedge.exe" `
  --app="file:///D:/你的路径/remote_process_control/frontend/index.html?rpcWindow=1&autostart=1&signaling=ws%3A%2F%2F192.168.1.10%3A9090%2F"
```

注意：`signaling=` 的值需 **URL 编码**（或用脚本自动生成）。

**说明**：`client.js` 里的 **`window.rpcShell.close()`** 在纯浏览器里通常无效（`window.close()` 受限）；关闭窗口请用 **Alt+F4** 或点窗口 **×**。应用模式下的 **自动关窗**（无视频超时等）在浏览器里会尝试 `window.close()`，**可能关不掉**，需用户手动关窗——若必须自动退出进程，请用下方 **WebView2 启动器**。

---

## 方案 B：WebView2 小启动器（Windows，需 .NET 8）

使用系统自带的 **WebView2 运行时**（Win10/11 多数已装），项目 **`webview2-launcher/`** 体积极小，**无需下载 Electron**。

```bash
cd webview2-launcher
dotnet run -- --signaling=ws://192.168.1.10:9090/
```

会通过脚本注入提供 **`window.rpcShell.close()`**，与原先 Electron 行为一致（含 Esc / 自动关窗逻辑）。

详见 **`webview2-launcher/README.md`**。

---

## 方案 C：继续用普通浏览器 + 应用模式 URL

直接打开或收藏：

`frontend/index.html?rpcWindow=1&autostart=1&signaling=ws://...`

仍带浏览器外框；适合调试。

---

## `frontend-vue-electron` 与 Electron

该目录已改为 **默认只跑 Vite（Vue）**，**不再依赖 Electron**。若网络畅通仍想用 Electron，可自行把 `electron` 装回并参照旧文档接 `electron/main.cjs`。
