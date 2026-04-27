// ============================================================
// capture_backend_factory.cpp
// ============================================================

#include "capture_backend_factory.h"
#include "dxgi_capture_backend.h"
#include "gdi_capture_backend.h"

#include "app/runtime_config.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>

namespace capture {

static std::string to_lower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

CaptureBackendKind resolve_backend_kind(std::string_view cfg,
                                         bool* out_unavailable) {
    if (out_unavailable) *out_unavailable = false;
    const std::string lower = to_lower(std::string(cfg));
    const bool dxgi_ok = DxgiCaptureBackend::probe();

    if (lower == "gdi")  return CaptureBackendKind::Gdi;

    if (lower == "dxgi") {
        if (!dxgi_ok) {
            if (out_unavailable) *out_unavailable = true;
            std::cout << "[factory] DXGI requested but unavailable\n";
            return CaptureBackendKind::Unknown;
        }
        return CaptureBackendKind::Dxgi;
    }

    if (lower == "wgc") {
        std::cout << "[factory] wgc backend is no longer supported\n";
        if (out_unavailable) *out_unavailable = true;
        return CaptureBackendKind::Unknown;
    }

    if (lower != "auto" && !lower.empty())
        std::cout << "[factory] unknown backend '" << lower << "', using auto\n";

    return dxgi_ok ? CaptureBackendKind::Dxgi : CaptureBackendKind::Gdi;
}

std::unique_ptr<ICaptureBackend> create_capture_backend(std::string_view cfg) {
    bool unavailable = false;
    const CaptureBackendKind kind = resolve_backend_kind(cfg, &unavailable);

    if (unavailable) {
        // 指定了不可用的后端：严格模式，不 fallback
        std::cout << "[factory] backend '" << cfg
                  << "' unavailable; strict mode, returning nullptr\n";
        return nullptr;
    }

    switch (kind) {
        case CaptureBackendKind::Dxgi:
            return std::make_unique<DxgiCaptureBackend>();
        case CaptureBackendKind::Gdi:
        default:
            return std::make_unique<GdiCaptureBackend>();
    }
}

std::unique_ptr<ICaptureBackend> create_capture_backend_from_config() {
    const std::string cfg = to_lower(runtime_config::get_string("RPC_CAPTURE_BACKEND", "auto"));
    return create_capture_backend(cfg);
}

} // namespace capture
