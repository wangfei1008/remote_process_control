#pragma once

// Compatibility layer for legacy `capture_worker.cpp/h` without modifying them.
// This header is injected via MSVC /FI (Forced Include Files) for capture_worker.cpp.

#include <utility>

#include "capture/bmp_dump_writer.h"
#include "capture/policy/capture_target_resolver.h"
#include "common/remote_video_contract.h"

class BmpDumpWriter;

// `capture_worker.*` uses an (otherwise undefined) `ResolveResult` type.
// Make it interoperate with `CaptureTargetResolver::resolve()` return type.
struct ResolveResult : public capture::CaptureTargetResult {
    ResolveResult() = default;
    ResolveResult(const capture::CaptureTargetResult& r) : capture::CaptureTargetResult(r) {}
    ResolveResult(capture::CaptureTargetResult&& r) noexcept : capture::CaptureTargetResult(std::move(r)) {}
};

inline void release_raw_frame_owned(rpc_video_contract::RawFrame& f)
{
    if (f.owned.release && f.owned.opaque) {
        f.owned.release(f.owned.opaque);
    }
    f.owned = {};
    f.plane_count = 0;
    for (auto& p : f.planes) p = {};
    if (f.gpu.release && f.gpu.opaque) {
        f.gpu.release(f.gpu.opaque);
    }
    f.gpu = {};
    f.ext = nullptr;
}

inline BmpDumpDiag make_bmp_dump_diag_from_hw(bool used_hw)
{
    BmpDumpDiag d;
    d.use_hw_capture = used_hw;
    d.force_software_active = false;
    d.top_black_strip_streak = 0;
    d.dxgi_instability_score = 0;
    d.dxgi_disabled_for_session = false;
    return d;
}

