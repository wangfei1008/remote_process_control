#include "capture/capture_source_factory.h"

#include "capture/dxgi_process_ui_capture_backend.h"
#include "capture/gdi_process_ui_capture_backend.h"

#include "common/character_conversion.h"
#include "app/runtime_config.h"

#include <iostream>


rpc_video_contract::CaptureBackend resolve_capture_backend(const std::string& b, bool* out_explicit_backend_unavailable)
{
    const bool dxgi_ok = DxgiProcessUiCaptureBackend::probe();
    if (out_explicit_backend_unavailable) *out_explicit_backend_unavailable = false;

    if (b == "gdi") {
        return rpc_video_contract::CaptureBackend::Gdi;
    }
    if (b == "dxgi") {
        if (!dxgi_ok) {
            if (out_explicit_backend_unavailable) *out_explicit_backend_unavailable = true;
            return rpc_video_contract::CaptureBackend::Unknown;
        }
        return rpc_video_contract::CaptureBackend::Dxgi;
    }
    if (b == "wgc") {
        std::cout << "[capture] RPC_CAPTURE_BACKEND=wgc is no longer supported (strict: session will not start).\n";
        if (out_explicit_backend_unavailable) *out_explicit_backend_unavailable = true;
        return rpc_video_contract::CaptureBackend::Unknown;
    }

    if (b != "auto" && !b.empty()) {
        std::cout << "[capture] unknown RPC_CAPTURE_BACKEND=" << b << ", using auto\n";
    }

    return dxgi_ok ? rpc_video_contract::CaptureBackend::Dxgi : rpc_video_contract::CaptureBackend::Gdi;
}


std::unique_ptr<ICaptureSource> create_capture_source()
{
    const std::string backend_cfg = to_lower_ascii(runtime_config::get_string("RPC_CAPTURE_BACKEND", "auto"));
    bool explicit_backend_unavailable = false;
    const rpc_video_contract::CaptureBackend resolved = resolve_capture_backend(backend_cfg, &explicit_backend_unavailable);
    if (explicit_backend_unavailable) {
        std::cout << "[capture] RPC_CAPTURE_BACKEND=" << backend_cfg << " unavailable at init; strict mode ¡ª no GDI fallback, capture backend not created.\n";
    }

    switch (resolved) {
        case rpc_video_contract::CaptureBackend::Dxgi:
            return std::make_unique<DxgiProcessUiCaptureBackend>();
        case rpc_video_contract::CaptureBackend::Gdi:
        default:
            return std::make_unique<GdiProcessUiCaptureBackend>();
    }
}
