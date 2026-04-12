#pragma once

#include <string>

/// 进程级固定的采集实现种类（初始化选定后不再因帧质量切换）。
enum class ProcessCaptureKind {
    Gdi,
    Dxgi,
};

struct CaptureKindResolveResult {
    ProcessCaptureKind kind = ProcessCaptureKind::Gdi;
    /// 显式指定 dxgi 但环境不可用（与 auto 无关）。
    bool explicit_backend_unavailable = false;
};

/// @param backend_cfg_lower 已转小写的 RPC_CAPTURE_BACKEND（auto/gdi/dxgi；wgc 已移除，显式 wgc 将 explicit_backend_unavailable）
/// AUTO 优先级：DXGI > GDI（由各后端 probe() 决定）。
CaptureKindResolveResult resolve_capture_kind(const std::string& backend_cfg_lower);
