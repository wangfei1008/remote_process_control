# 架构说明（前端 / 信令 / Windows 客户端）

本文描述三端分层、设计模式与目录约定，便于后续演进（鉴权、房间、配置化等）。

## 总览

```
浏览器 (前端)  <--WebSocket 信令-->  Qt 信令服务器  <--WebSocket 信令-->  Windows 客户端
     ^                                                               |
     +-------------------- WebRTC (SRTP / DataChannel) --------------+
```

- **信令**：仅转发 JSON（`id` 为目标端标识），不解析 SDP 语义。
- **媒体与控制**：WebRTC；控制走 DataChannel。

---

## 前端（`frontend/`）

### 结构

- **`client.js`**：单文件 IIFE，内含会话状态、信令、WebRTC、输入映射与 DOM 绑定（无 `import`，兼容 **`file://`**）。
- **`index.html`** / **`style.css`**：页面与样式。

### 模式（逻辑上仍可按层理解）

- **Facade**：`RemoteProcessApplication` 原型方法组装 WebSocket + RTCPeerConnection + DataChannel。
- **会话状态**：`createSession()` 返回的 plain object 在页面生命周期内共享。
- **工厂函数**：`createWebRtcSessionController`、`createSignalingClient` 在闭包内注入 `session` / `document` / `ui`。

### 加载方式

- `index.html` 使用 `<script src="./client.js" defer>`；可在 **Chrome 下 `file://` 双击打开**。

---

## 信令服务器（`signaling-server-qt/`）

### 分层

| 组件 | 文件 | 模式 / 职责 |
|------|------|-------------|
| **ClientRegistry** | `core/ClientRegistry.*` | **注册表**：`id → QWebSocket*` |
| **SignalingRelay** | `core/SignalingRelay.*` | **策略 / 路由**：解析 JSON、改写 `id` 为来源、转发给目标 |
| **SignalingServer** | `SignalingServer.*` | **门面**：监听连接、生命周期、日志；委托 Registry + Relay |

### 扩展建议

- 鉴权、房间：在 `SignalingRelay::relayTextMessage` 前增加 **责任链** 或 **中间件**。
- 指标：对 `relayTextMessage` 包装 **装饰器** 记录延迟与字节数。

---

## Windows 客户端（`remote_process_control/`）

### 逻辑分层（当前以文件为界，未强制子目录）

| 层 | 典型单元 | 职责 |
|----|----------|------|
| **信令与会话** | `webrtc_socket`, `client_peer_connection`, `dispatch_queue` | WebSocket、PeerConnection、每客户端轨道与 DataChannel 消息 |
| **媒体管线** | `stream`, `process_manager`, `window_capture`, `h264_encoder`, … | 采集、编码、按时间片送样 |
| **输入适配** | `input_controller` | `SendInput`、坐标映射、前台窗口 |
| **入口** | `main.cpp` | 进程入口、启动 `WebRTCSocket` |

### 模式（现有代码体现）

- **每连接对象**：`ClientPeerConnection` 封装单个浏览器端的信令与媒体状态。
- **单线程队列**：`DispatchQueue` 将 WebRTC 回调派发到固定线程，减少竞态。
- **单例**（输入）：`InputController::instance()` 与 OS 全局输入子系统对应。

### 物理目录重构（可选后续）

若需与文档完全一致，可将源码迁至：

- `signaling/`：`webrtc_socket`, `client_peer_connection`, `dispatch_queue`
- `media/`：`stream`, `process_manager`, 采集与编码相关文件
- `input/`：`input_controller`
- `app/`：`main.cpp`

并在 VS 工程中配置 **附加包含目录** 为各子目录，避免 `#include` 大规模改名。

---

## 版本与兼容

- 前端依赖 **现代浏览器**（WebRTC + ES Modules）。
- Qt 工程需 **Qt WebSockets**；C++ 标准见各 `.pro` / `.vcxproj`。
