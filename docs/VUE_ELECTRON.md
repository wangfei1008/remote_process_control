# Vue / Electron 说明（当前仓库）

当前仓库仅包含原生前端目录 `frontend/`，不包含 `frontend-vue-electron/` 或 Electron 壳工程。

## 结论

- 运行本项目不需要 Vue，也不需要 Electron。
- 推荐直接使用 `frontend/index.html`（可 `file://` 打开）。
- 若需要“应用窗口”体验，使用浏览器 `--app` 方式，见 `WITHOUT_ELECTRON.md`。

---

## 如果你要自建 Vue 前端

可以新建独立目录（例如 `frontend-vue/`），按以下思路迁移：

1. 保留现有信令与 WebRTC 协议不变（消息结构保持兼容）。
2. 将 `frontend/client.js` 中的状态与流程拆成模块（session/signaling/webrtc/ui）。
3. “我的数据”页面可先 iframe 复用 `my_data.html`，再逐步组件化重写。
4. 保持 URL 参数语义一致（`rpcWindow`、`autostart`、`signaling`）。

---

## 如果你要自建 Electron 壳

- 建议新建独立工程目录，加载 `frontend/index.html`。
- 通过 preload 注入桥接对象（如 `window.rpcShell.close()`）以支持可控关窗。
- 保证前端在“无桥接环境”可降级运行（浏览器直接打开仍可用）。
