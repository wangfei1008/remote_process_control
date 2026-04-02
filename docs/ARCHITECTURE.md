# 架构说明（当前仓库版本）

本文按当前代码结构说明前端、信令与 Windows 客户端协作方式，内容以仓库现状为准。

## 总览

```
前端（浏览器） <--WebSocket 信令--> Qt 信令服务 <--WebSocket 信令--> rpc_remote_client
      ^                                                           |
      +---------------- WebRTC(视频轨 + DataChannel) -------------+
```

- 信令层仅做 JSON 中继（按 `id` 路由）。
- 媒体与输入控制都走 WebRTC。
- 文件上传下载复用 DataChannel，和远程控制共用连接体系。

---

## 前端（`frontend/`）

### 主要文件

- `index.html`：主页面，包含连接面板、状态面板、远程视频层。
- `desktopMode.js`：桌面容器、多窗口管理、应用图标（含“我的数据”）。
- `my_data.html` / `my_data.js`：远端文件浏览、预览、上传、下载、传输日志。
- `client.js`：会话状态与入口初始化（URL 参数、自动连接、桌面模式对接）。
- `signalingLayer.js` / `webrtcLayer.js` / `uiLayer.js`：按职责拆分的前端逻辑层。

### 运行特性

- 支持 `file://` 直接打开 `frontend/index.html`。
- 支持 `rpcWindow=1`/`kiosk=1`、`autostart`、`signaling` 等 URL 参数。
- “我的数据”通过 DataChannel 与后端 `FileTransferService` 通信。

---

## 信令服务（`signaling-server-qt/`）

### 主要组件

| 组件 | 文件 | 职责 |
|------|------|------|
| ClientRegistry | `core/ClientRegistry.*` | 维护 `id -> websocket` 映射 |
| SignalingRelay | `core/SignalingRelay.*` | 校验并转发信令消息 |
| SignalingServer | `SignalingServer.*` | 监听连接与生命周期管理 |
| 程序入口 | `main.cpp` | 默认监听 `0.0.0.0:9090`，支持命令行 host/port |

---

## Windows 远端客户端（`rpc_remote_client/`）

### 分层

| 层 | 典型文件 | 职责 |
|----|----------|------|
| 连接与会话 | `transport/webrtc_socket.*`, `transport/client_peer_connection.*` | 信令收发、PeerConnection、DataChannel 消息分发 |
| 文件传输 | `transport/file_transfer_service.*` | 文件列表/预览/上传/下载、分片与偏移、路径安全校验 |
| 媒体采集与编码 | `capture/*`, `encode/*`, `session/*`, `source/*` | 画面采集、H264 编码、媒体流输出 |
| 输入控制 | `input/input_controller.*` | 鼠标键盘注入 |
| 配置与入口 | `common/runtime_config.h`, `main.cpp` | 读取 `rpc_config.ini`、启动 WebRTC Socket |

### 关键实现说明

- 文件根目录由 `RPC_DATA_ROOT` 限定，路径会校验“必须在根目录内”。
- 分片大小由 `RPC_FILE_CHUNK_SIZE` 控制，前后端需保持一致量级。
- DataChannel 心跳仅匹配纯 `Pong` 文本，避免误伤文件分片消息。

---

## 兼容与依赖

- 前端：现代浏览器（WebRTC/DataChannel）。
- 信令：Qt + Qt WebSockets。
- 远端客户端：Windows + Visual Studio C++ 工具链。
