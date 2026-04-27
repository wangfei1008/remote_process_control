#pragma once

// ============================================================
// capture_backend_factory.h
// 采集后端工厂：根据配置创建合适的后端实例
// ============================================================

#include "i_capture_backend.h"
#include <memory>
#include <string_view>

namespace capture {

// 根据配置字符串（"auto"/"dxgi"/"gdi"）解析后端类型
// out_unavailable: 若指定了一个当前不可用的后端，设为 true
CaptureBackendKind resolve_backend_kind(std::string_view cfg,
                                         bool* out_unavailable = nullptr);

// 创建后端实例
// - "auto"  → 优先 DXGI，不可用则 GDI
// - "dxgi"  → 强制 DXGI，不可用则返回 nullptr（严格模式）
// - "gdi"   → 强制 GDI
// 返回 nullptr 表示指定后端不可用且不允许 fallback
std::unique_ptr<ICaptureBackend> create_capture_backend(std::string_view cfg = "auto");

// 从运行时配置读取 cfg 并创建（读取 RPC_CAPTURE_BACKEND）
std::unique_ptr<ICaptureBackend> create_capture_backend_from_config();

} // namespace capture
