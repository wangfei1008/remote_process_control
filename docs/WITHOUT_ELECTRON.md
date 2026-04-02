# 不用 Electron 的独立窗口方案（当前仓库）

当前仓库不包含 Electron 工程。若需要“像独立应用一样打开远程画面”，建议使用系统浏览器 `--app` 模式。

## 方案：Edge / Chrome `--app`

### 1) 直接打开本地页面

```powershell
& "$env:ProgramFiles (x86)\Microsoft\Edge\Application\msedge.exe" `
  --app="file:///D:/code/orient/remote_process_control/frontend/index.html?rpcWindow=1&autostart=1&signaling=ws%3A%2F%2F127.0.0.1%3A9090%2F"
```

如果系统安装的是 Chrome，可替换成对应 `chrome.exe` 路径。

### 2) 参数说明

- `rpcWindow=1`：应用模式，隐藏大部分控制台 UI，仅保留远程画面层。
- `autostart=1`：页面加载后自动尝试连接。
- `signaling=...`：信令地址，建议做 URL 编码后再拼接。

---

## 注意事项

- `--app` 能去掉标签栏与地址栏，但无法去掉系统窗口边框。
- 纯浏览器环境下，`window.close()` 常受限制，自动关窗不一定成功。
- 若需要“可控关窗、无边框、自定义系统交互”，建议在本仓库之外自建 WebView2/Electron 壳。

---

## 调试模式（非应用窗口）

也可以直接在浏览器打开：

`frontend/index.html?rpcWindow=1&autostart=1&signaling=ws://127.0.0.1:9090/`

该方式更适合排查连接和日志问题。
