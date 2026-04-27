#pragma once

// ============================================================
// capture_backend_kind.h
// 后端类型枚举（原 rpc_video_contract::CaptureBackend）
// ============================================================

namespace capture {

enum class CaptureBackendKind {
    Unknown,
    Dxgi,
    Gdi,
};

} // namespace capture
