# remote_process_control

基于 WebRTC 的远程进程控制与文件传输工程，包含：
- 前端桌面容器（支持远程画面窗口与“我的数据”文件管理）
- Qt 信令中继服务
- Windows 远端客户端 `rpc_remote_client`（采集/编码/输入注入/文件传输）

---

## 目录结构

- `frontend/`：前端页面与桌面容器
  - `index.html`：入口
  - `desktopMode.js`：桌面、多窗口、任务栏
  - `my_data.html` / `my_data.js`：“我的数据”文件浏览/上传/下载
- `signaling-server-qt/`：WebSocket 信令服务（按 id 转发）
- `rpc_remote_client/`：Windows 远端客户端（VS 工程）
- `remote_desktop_receiver/`：独立接收端（WebRTC Answerer + H264 解码 + D3D 呈现 + 输入回传）
- `docs/`：架构与运行说明文档

---

## 核心能力

### 1) 远程进程画面与控制
- 前端发起 `request`，远端启动目标进程并采集窗口
- 远端编码 H264，通过 WebRTC 视频轨下发
- 鼠标/键盘通过 DataChannel 上行，远端注入输入

### 2) “我的数据”文件管理
- 前端桌面可打开“我的数据”窗口
- 浏览远端固定目录（`RPC_DATA_ROOT`）
- 支持文件预览（文本类）
- 支持下载与上传
- 上传支持断点协议字段，当前默认按“全新上传”执行（`resume=false`）

### 3) 传输与稳定性
- 文件传输走 DataChannel（JSON + base64 分片）
- 分片大小通过 `RPC_FILE_CHUNK_SIZE` 配置
- 前端带 ACK 等待、超时重试、偏移对齐
- 后端带路径安全校验（限制在根目录内）

---

## 快速启动

### 1. 启动信令服务
编译并运行 `signaling-server-qt`，默认地址：
- `ws://127.0.0.1:9090`

### 2. 启动远端客户端
在 Visual Studio 打开 `remote_process_control.sln`：
- 启动项目：`rpc_remote_client`
- 工程文件：`rpc_remote_client/rpc_remote_client.vcxproj`

当前 `rpc_remote_client/main.cpp` 默认连接：
- `127.0.0.1:9090`

### 3. 打开前端
- 直接打开 `frontend/index.html`（`file://` 方式可用）
- 或用静态服务器访问 `frontend/`

---

## 关键配置

配置文件：`rpc_remote_client/rpc_config.ini`

常用项：
- `RPC_CAPTURE_BACKEND=dxgi`：采集后端（dxgi/gdi）。默认 `dxgi`；若系统不支持 dxgi 则退回 gdi
- `RPC_ACTIVE_FPS=30`：活跃帧率
- `RPC_IDLE_FPS=5`：空闲帧率
- `RPC_ENCODER_BACKEND=auto`：编码后端
- `RPC_DATA_ROOT=D:\rpc_data`：“我的数据”根目录
- `RPC_FILE_CHUNK_SIZE=4096`：文件分片大小（字节）

---

## 文件传输消息（简）

前端 -> 远端：
- `fileList`
- `filePreview`
- `fileDownloadInit` / `fileDownloadChunk`
- `fileUploadInit` / `fileUploadChunk` / `fileUploadCommit`

远端 -> 前端：
- `fileListResult`
- `filePreviewResult`
- `fileDownloadReady` / `fileDownloadChunkData`
- `fileUploadReady` / `fileUploadAck` / `fileUploadCommitted`
- `fileError`

---

## 近期实现注意点

- 工程目录已从旧的 `remote_process_control/` 子目录迁移为 `rpc_remote_client/`
- DataChannel 心跳判断已修复为仅匹配纯 `Pong`，避免误吞文件分片
- 文件路径使用 UTF-8 路径转换，降低中文文件名兼容问题
- 上传默认不复用旧 `.rpcpart`，避免固定偏移卡住

---

## 常见排查

### 看不到“我的数据”文件
1. 确认 `RPC_DATA_ROOT` 指向真实存在目录
2. 重启 `rpc_remote_client` 使配置生效
3. 查看前端日志是否出现“已加载根目录: ...”

### 大文件上传中断
1. 确认前后端为最新代码
2. 检查 `rpc_remote_client` 控制台中的 `[file][upload_*]` 日志
3. 检查前端“传输日志”是否有重试或 `fileError`

---

## 文档

- `docs/ARCHITECTURE.md`
- `docs/WITHOUT_ELECTRON.md`
- `docs/VUE_ELECTRON.md`
