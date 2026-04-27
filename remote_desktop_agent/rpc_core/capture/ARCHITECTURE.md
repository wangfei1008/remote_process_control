# Capture 子系统架构文档

## 目录结构

```
capture/
├── capture.h                        ← 统一入口（外部只需包含这一个）
│
├── infra/                           ← Win32 基础设施层
│   ├── win32_types.h                  HANDLE/ProcessInfoRaii RAII
│   ├── win32_process.h/.cpp           进程工具（无状态，全 const）
│   ├── win32_window.h/.cpp            窗口工具（无状态，全 const）
│   └── gdi_resource_guards.h          GDI RAII 守卫
│
├── session/                         ← 会话状态层
│   └── process_session.h/.cpp         进程生命周期（start/stop/rebind）
│
├── policy/                          ← 策略层（无 Win32，可单元测试）
│   ├── window_score_policy.h/.cpp     窗口评分（可继承覆盖）
│   └── capture_target_resolver.h/.cpp 采集目标解析（const，无副作用）
│
├── backend/                         ← 采集后端
│   ├── capture_backend_kind.h         后端类型枚举
│   ├── window_tile.h                  单窗口瓦片数据结构
│   ├── i_capture_backend.h            后端接口（精简版）
│   ├── dxgi_capture_backend.h/.cpp    DXGI Desktop Duplication
│   ├── gdi_capture_backend.h/.cpp     GDI 回退后端
│   └── capture_backend_factory.h/.cpp 工厂（按配置创建）
│
└── pipeline/                        ← 管线层
    ├── frame_filter.h/.cpp            可疑帧过滤（纯像素运算）
    ├── frame_composer.h/.cpp          多窗口帧合成（纯像素运算）
    └── capture_pipeline.h/.cpp        管线编排（组合上面所有东西）
```

## 层次规则

```
外部调用方
    │
    ▼
capture_pipeline          依赖 backend + composer + filter
    │
    ├── ICaptureBackend   依赖 win32::Window（仅用于 rect 查询）
    │
    ├── FrameComposer     无 Win32 依赖，纯内存操作
    │
    └── FrameFilter       无 Win32 依赖，纯像素统计
    
CaptureTargetResolver     依赖 win32::Window + win32::Process + WindowScorePolicy
    │                     const resolve()，无副作用
    └── WindowScorePolicy 依赖 win32::WindowInfo（数据），不调用 Win32 API

ProcessSession            依赖 win32::Process（内部）
                          暴露 rebind_capture_pid()，由调用方在接受 resolver 建议后调用

win32::Process            无状态，全 const，可自由传递
win32::Window             无状态，全 const，可自由传递
```

## 与原代码的关键差异

| 原代码 | 重构后 | 原因 |
|--------|--------|------|
| `process_ops`：会话 + 工具混合 | `ProcessSession`（会话）+ `win32::Process`（工具）分离 | 不同生命周期，不同测试方式 |
| `CaptureTargetResolver::resolve()` 直接写 `proc.set_capture_pid()` | `resolve()` 为 const，返回建议，调用方决定是否接受 | 副作用不应混入查询 |
| 评分魔法数字硬编码在匿名 namespace | `WindowScorePolicy` 独立类，方法可覆盖 | 策略应可测试、可替换 |
| GDI 多路径手工资源释放 | `DcGuard`/`CompatDcGuard`/`BitmapGuard` RAII | 任意返回路径均安全 |
| `ICaptureSource::init()` 虚函数 | 删除，构造函数负责初始化 | 调用时机不明确是 bug 来源 |
| `reset_session_recovery()` 名字不说时机 | `on_new_session()` | 名字即文档 |
| `compose_bbox/linear` 和采集逻辑混一文件 | `FrameComposer` 独立，纯内存操作 | 可独立单元测试 |
| `capture_rgb_heuristics` 自由函数 | `FrameFilter` 类，`should_discard()` 说清意图 | 调用方清楚"为什么调" |

## 扩展点

- **自定义评分策略**：继承 `WindowScorePolicy`，覆盖 `style_score()` / `splash_tokens()` 等
- **新采集后端**：实现 `ICaptureBackend`，在 `capture_backend_factory.cpp` 注册
- **新合成布局**：在 `CompositeLayout` 枚举添加值，在 `FrameComposer::compose_linear()` 处理
