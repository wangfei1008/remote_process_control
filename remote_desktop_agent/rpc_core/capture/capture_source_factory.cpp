#include "capture/capture_source_factory.h"

#include "capture/dxgi_process_ui_capture_backend.h"
#include "capture/gdi_process_ui_capture_backend.h"

std::unique_ptr<ICaptureSource> create_capture_source(ProcessCaptureKind kind)
{
    switch (kind) {
        case ProcessCaptureKind::Dxgi:
            return std::make_unique<DxgiProcessUiCaptureBackend>();
        case ProcessCaptureKind::Gdi:
        default:
            return std::make_unique<GdiProcessUiCaptureBackend>();
    }
}
