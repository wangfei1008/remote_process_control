#include "capture/capture_kind_resolver.h"

#include "capture/dxgi_process_ui_capture_backend.h"

#include <iostream>

CaptureKindResolveResult resolve_capture_kind(const std::string& b)
{
    const bool dxgi_ok = DxgiProcessUiCaptureBackend::probe();

    CaptureKindResolveResult r;

    if (b == "gdi") {
        r.kind = ProcessCaptureKind::Gdi;
        return r;
    }
    if (b == "dxgi") {
        if (!dxgi_ok) {
            r.explicit_backend_unavailable = true;
            return r;
        }
        r.kind = ProcessCaptureKind::Dxgi;
        return r;
    }
    if (b == "wgc") {
        r.explicit_backend_unavailable = true;
        std::cout << "[capture] RPC_CAPTURE_BACKEND=wgc is no longer supported (strict: session will not start).\n";
        return r;
    }

    if (b != "auto" && !b.empty()) {
        std::cout << "[capture] unknown RPC_CAPTURE_BACKEND=" << b << ", using auto\n";
    }

    if (dxgi_ok) {
        r.kind = ProcessCaptureKind::Dxgi;
    } else {
        r.kind = ProcessCaptureKind::Gdi;
    }
    return r;
}
