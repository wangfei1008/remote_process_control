// ============================================================
// capture_pipeline.cpp
// ============================================================

#include "capture_pipeline.h"
#include "common/rpc_time.h"

#include <iostream>

namespace capture {

// ---- 内部工具 -----------------------------------------------
namespace {

void release_rgb_vector(void* opaque) {
    delete static_cast<std::vector<uint8_t>*>(opaque);
}

rpc_video_contract::CaptureBackend contract_backend(CaptureBackendKind k)
{
    switch (k) {
    case CaptureBackendKind::Dxgi: return rpc_video_contract::CaptureBackend::Dxgi;
    case CaptureBackendKind::Gdi:  return rpc_video_contract::CaptureBackend::Gdi;
    default: return rpc_video_contract::CaptureBackend::Unknown;
    }
}

} // namespace

// ---- 构造 ---------------------------------------------------

CapturePipeline::CapturePipeline(Deps deps) : m_deps(deps) {}

// ---- grab（中间层，不涉及合约） -----------------------------

GrabResult CapturePipeline::grab(
    std::span<const win32::WindowInfo> surfaces,
    uint64_t                           now_unix_ms,
    const FrameComposer::Options&      opts,
    bool                               filter_black) const
{
    GrabResult result;
    result.telem.now_unix_ms = now_unix_ms;
    result.telem.backend     = m_deps.backend.kind();

    if (surfaces.empty()) {
        std::cout << "[pipeline] no surfaces\n";
        return result;
    }

    // ---- 采集瓦片 -------------------------------------------
    std::vector<WindowTile> tiles;
    if (!m_deps.backend.capture_tiles(surfaces, tiles, now_unix_ms)) {
        std::cout << "[pipeline] capture_tiles failed\n";
        return result;
    }

    // ---- 合成 -----------------------------------------------
    result.frame = m_deps.composer.compose(tiles, opts);
    if (!result.frame.ok || result.frame.pixels.empty()
        || result.frame.width <= 0 || result.frame.height <= 0) {
        std::cout << "[pipeline] compose failed\n";
        result.frame = {};
        return result;
    }

    // ---- 可疑帧过滤 -----------------------------------------
    if (filter_black && m_deps.filter.should_discard(
            result.frame.pixels, result.frame.width, result.frame.height)) {
        std::cout << "[pipeline] discarded suspicious frame (e.g. black startup)\n";
        result.frame = {};
        return result;
    }

    result.telem.cap_unix_ms = rpc_unix_epoch_ms();
    result.ok = true;
    return result;
}

// ---- grab_raw_frame（填充合约） -----------------------------

bool CapturePipeline::grab_raw_frame(
    std::span<const win32::WindowInfo>     surfaces,
    uint64_t                               now_unix_ms,
    uint64_t                               prep_unix_ms,
    uint64_t                               frame_id,
    bool                                   filter_black,
    rpc_video_contract::RawFrame&          out_frame,
    rpc_video_contract::TelemetrySnapshot& out_telem) const
{
    out_frame = rpc_video_contract::RawFrame{};
    out_telem = rpc_video_contract::TelemetrySnapshot{};

    // 无论成败都填写遥测基础字段
    out_telem.frame_unix_ms = static_cast<rpc_video_contract::TimeUs>(now_unix_ms);
    out_telem.prep_unix_ms  = static_cast<rpc_video_contract::TimeUs>(prep_unix_ms);
    out_telem.backend       = contract_backend(m_deps.backend.kind());
    out_telem.frame_id      = frame_id;

    const FrameComposer::Options opts = FrameComposer::options_from_config();
    GrabResult r = grab(surfaces, now_unix_ms, opts, filter_black);
    if (!r.ok) return false;

    const ComposedFrame& f = r.frame;

    // ---- 填充遥测 -------------------------------------------
    out_telem.capture_size    = rpc_video_contract::VideoSize{ f.width, f.height };
    out_telem.capture_unix_ms =
        static_cast<rpc_video_contract::TimeUs>(r.telem.cap_unix_ms);

    // ---- 填充 RawFrame（零拷贝移动像素 buffer）--------------
    auto* vec = new std::vector<uint8_t>(std::move(
        const_cast<ComposedFrame&>(f).pixels));

    out_frame.frame_id    = frame_id;
    out_frame.pts_us      =
        static_cast<rpc_video_contract::TimeUs>(r.telem.cap_unix_ms) * 1000;
    out_frame.dts_us      = out_frame.pts_us;
    out_frame.coded_size  = rpc_video_contract::VideoSize{ f.width, f.height };
    out_frame.visible_rect =
        rpc_video_contract::VideoRect{ f.origin_left, f.origin_top, f.width, f.height };
    out_frame.display_size   = rpc_video_contract::VideoSize{};
    out_frame.format         = rpc_video_contract::PixelFormat::RGB24;
    out_frame.rotation       = rpc_video_contract::Rotation::R0;
    out_frame.is_screen_content = true;
    out_frame.storage        = rpc_video_contract::FrameStorageKind::Cpu;

    out_frame.owned.bytes.data = vec->data();
    out_frame.owned.bytes.size = static_cast<uint32_t>(vec->size());
    out_frame.owned.opaque     = vec;
    out_frame.owned.release    = &release_rgb_vector;

    out_frame.plane_count      = 1;
    out_frame.planes[0].data   = vec->data();
    out_frame.planes[0].stride_bytes = f.width * 3;
    out_frame.planes[0].size_bytes   = static_cast<uint32_t>(vec->size());

    return true;
}

} // namespace capture
