# 使用 Vue + 独立窗口：是否必须用 Electron？

**不必。** Electron 只是选项之一；在国内网络下 **Electron 安装容易卡住**，推荐优先：

- **`scripts/open-remote-app.ps1`** — 系统 **Edge/Chrome `--app`** 模式（见 **`WITHOUT_ELECTRON.md`**）
- **`webview2-launcher/`** — **.NET + WebView2**，体积小

## Vue 与本仓库

- **`frontend-vue-electron/`**：**Vue 3 + Vite**，默认 **不含 Electron**，仅做开发与打包；页面仍加载 **`../frontend/client.js`**。
- 若网络畅通且需要 Electron 生态，可自行安装 Electron 或使用 **[electron-vite](https://github.com/electron-vite/electron-vite)** 模板。

## 技术注意点

- **单文件 `client.js`** 不是 ES 模块时，在 Vue 里继续用 `<script src="/client.js" defer>`，或逐步改成 `export` 模块再 `import`。
- **应用模式**（`rpcWindow=1`）、**自动关窗** 等逻辑在 **`client.js`**；**`window.rpcShell.close()`** 仅在 **WebView2 启动器** 或 **Electron** 等注入桥接时有效，纯 `--app` 浏览器可能无法 `window.close()`，见 **`WITHOUT_ELECTRON.md`**。
