# remote_desktop_agent：主要业务流程与数据流

本文档与 `remote_desktop_agent` 当前实现一致，便于评审与 onboarding。

---

## 1. 主要业务流程图（生命周期 + 信令驱动）

```mermaid
flowchart TB
    subgraph 启动
        M[main]
        A[rpc_remote_application 构造]
        M --> A
        A --> RS[runtime_settings::load_from_environment]
        A --> T[signaling_transport]
        A --> D[session_director]
        A --> O[transport.set_observer: director]
        A --> C[transport.set_on_transport_closed: stop_all_sessions]
    end

    subgraph 运行
        R[run: transport.start 连接信令]
        R --> W[阻塞 stdin 等待退出]
    end

    subgraph 信令入站
        WS[WebSocket 收 JSON]
        WS --> P[解析为 signaling_event]
        P --> E[observer.on_signaling_event]
        E --> Q[m_task_queue.dispatch 串行执行]
    end

    subgraph director 分支 on_signaling_event
        Q --> SW{event.type}
        SW -->|media_session_requested| RP[replace_with_new_session<br/>exe + media=true]
        SW -->|file_only_session_requested| RF[replace_with_new_session<br/>无 exe + media=false]
        SW -->|sdp_answer| AN{会话且 client_id 匹配?}
        AN -->|是| AA[active_session.apply_remote_answer]
        AN -->|否| IGN1[忽略]
        SW -->|stop_session| TD[teardown_active]
        SW -->|invalid| IGN2[忽略]
    end

    subgraph 新会话替换 replace_with_new_session
        RP --> TD0[teardown_active]
        RF --> TD0
        TD0 --> FAC[desktop_session_factory.create_session]
        FAC --> ADS[active_desktop_session::wire_components]
    end

    subgraph wire_components 装配
        ADS --> FS[FileTransferService]
        ADS --> MP{media_enabled?}
        MP -->|是| MPL[media_pipeline]
        MP -->|否| OP0
        MPL --> OP0[operator_channel + start_local_negotiation]
        MPL --> ATT[m_media.attach_operator]
    end

    subgraph 拆除 teardown_active / teardown
        TD --> MV[move m_active_session]
        MV --> TE[session.teardown]
        TE --> BR[broadcast remoteProcessExited]
        TE --> ST[m_media.stop_stream]
        TE --> RB[release_mouse_target_binding]
        TE --> CL[operator_channel.close]
    end

    subgraph 异常路径
        LOST[operator_channel 连接丢失] --> OCL[on_operator_connection_lost]
        OCL --> Q
        RPE[远端进程退出] --> HRP[handle_remote_process_exit]
        HRP --> PST[post_to_signaling: 广播 + 停流等]
    end

    subgraph 退出
        W --> SA[director.stop_all_sessions]
        SA --> Q2[m_task_queue: teardown_active]
    end
```

**要点（与代码一致）**

- 全局**至多一个** `m_active_session`；新 `media_session_requested` / `file_only_session_requested` 先 **`teardown_active` 再建会话**（`always_replace_session_policy` 可扩展是否允许替换）。
- **SDP Answer** 仅当 `event.client_id == m_current_client_id` 时交给当前会话。
- **编排与多数回调**经 `session_director` 的 `DispatchQueue`（`m_task_queue`）串行化，避免与 libdatachannel 回调线程直接交织。

---

## 2. 数据流图（信令 / WebRTC / 媒体与控制）

```mermaid
flowchart LR
    subgraph 远端
        UI[浏览器 / 控制台]
    end

    subgraph 信令面
        SIG[信令服务器 WebSocket]
    end

    subgraph Agent_信令层
        ST[signaling_transport]
        ST --> EV[signaling_event]
        EV --> DIR[session_director]
    end

    subgraph Agent_会话门面
        ADS[active_desktop_session]
        MPL[media_pipeline]
        OP[operator_channel]
        CPC[ClientPeerConnection]
        FTS[FileTransferService]
    end

    subgraph Agent_媒体管线_rpc_core
        RPS[RemoteProcessStreamSource]
        STR[Stream]
        VEP[video_encode_pipeline 等]
        SND[media_sender / 轨]
    end

    UI <-->|JSON: 会话请求 / SDP Answer / Stop| SIG
    SIG <-->|文本帧| ST
    DIR -->|send_signaling_json| ST
    ST -->|JSON Offer / ICE 等| SIG

    DIR --> ADS
    ADS --> FTS
    ADS --> MPL
    ADS --> OP
    OP --> CPC

    MPL --> RPS
    MPL --> STR
    RPS --> VEP
    STR --> SND
    SND --> CPC
    CPC -->|RTP 音视频| UI

    CPC -->|DataChannel: 键鼠·文件·remoteProcessExited| UI
    FTS <-->|分块与元数据| CPC

    RPS -.->|窗口/帧| VEP
```

**数据语义简述**

| 方向 | 载体 | 内容 |
|------|------|------|
| 入站信令 | WebSocket 文本 | 会话请求、SDP Answer、`stop` 等 → `signaling_event` |
| 出站信令 | WebSocket 文本 | Offer、ICE 候选等（由 `operator_channel` / `PeerConnection` 驱动，`send_signaling_json`） |
| 媒体 | RTP（经 libdatachannel） | 编码后的视频；静音 Opus 等 |
| 控制 / 侧车 | DataChannel JSON | 远程输入、文件传输协议、`type=remoteProcessExited` 广播 |
| 本地 | 共享内存 / 回调 | 采集 → 编码 → 入轨；进程生命周期与窗口选择 |

---

## 3. 与源码对应关系（快速索引）

| 节点 / 概念 | 主要文件 |
|-------------|----------|
| 应用装配与退出 | `app/main.cpp`, `app/rpc_remote_application.cpp` |
| 信令解析与线程收敛 | `signaling/signaling_transport.cpp`, `signaling/signaling_event.h` |
| 单会话 + 替换策略 | `orchestration/session_director.cpp`, `orchestration/session_replace_policy.h` |
| 会话装配与拆除 | `orchestration/active_desktop_session.cpp`, `orchestration/desktop_session_factory.cpp` |
| 单 Peer WebRTC | `webrtc/operator_channel.cpp` |
| 媒体组装 | `media/media_pipeline.cpp` |
| 采集 / 编码 / 发送实现 | `rpc_core/**` |

如需导出为图片，可在本地使用 [Mermaid CLI](https://github.com/mermaid-js/mermaid-cli) 或支持 Mermaid 的 Markdown 预览工具渲染本文档。
