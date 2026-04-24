#include "capture/process_ui_capture.h"

#include "app/runtime_config.h"
#include "capture/i_capture_source.h"
#include "capture/process_ui_tile.h"
#include "common/rpc_time.h"
#include "common/window_ops.h"

#include "capture/capture_rgb_heuristics.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <vector>

namespace {

static void release_vector_u8(void* opaque)
{
    delete static_cast<std::vector<uint8_t>*>(opaque);
}

void make_even(int& v)
{
    if (v > 0) v &= ~1;
}

CaptureGrabOutcome compose_bbox(const std::vector<ProcessUiWindowTile>& tiles)
{
    CaptureGrabOutcome outcome;
    if (tiles.empty()) return outcome;

    int min_left = INT_MAX;
    int min_top = INT_MAX;
    int max_right = INT_MIN;
    int max_bottom = INT_MIN;
    for (const auto& t : tiles) {
        min_left = (std::min)(min_left, t.origin_left);
        min_top = (std::min)(min_top, t.origin_top);
        max_right = (std::max)(max_right, t.origin_left + t.w);
        max_bottom = (std::max)(max_bottom, t.origin_top + t.h);
    }

    RECT clip_rect{min_left, min_top, max_right, max_bottom};
    int total_width = clip_rect.right - clip_rect.left;
    int total_height = clip_rect.bottom - clip_rect.top;
    make_even(total_width);
    make_even(total_height);
    if (total_width <= 0 || total_height <= 0) return outcome;

    outcome.width = total_width;
    outcome.height = total_height;
    outcome.cap_min_left = clip_rect.left;
    outcome.cap_min_top = clip_rect.top;

    std::vector<ProcessUiWindowTile> ordered = tiles;
    std::sort(ordered.begin(), ordered.end(), [](const ProcessUiWindowTile& a, const ProcessUiWindowTile& b) {
        return a.z_order < b.z_order;
    });

    std::vector<uint8_t> merged(static_cast<size_t>(total_width) * static_cast<size_t>(total_height) * 3u, 0);
    for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) {
        const ProcessUiWindowTile& win = *it;
        RECT win_rect{win.origin_left, win.origin_top, win.origin_left + win.w, win.origin_top + win.h};
        RECT inter{};
        if (!IntersectRect(&inter, &win_rect, &clip_rect)) continue;

        const int src_w = win.w;
        const int src_x = inter.left - win.origin_left;
        const int src_y = inter.top - win.origin_top;
        const int dst_x = inter.left - clip_rect.left;
        const int dst_y = inter.top - clip_rect.top;
        const int inter_w = inter.right - inter.left;
        const int inter_h = inter.bottom - inter.top;
        if (inter_w <= 0 || inter_h <= 0 || src_w <= 0) continue;

        for (int y = 0; y < inter_h; ++y) {
            const size_t src_row = (static_cast<size_t>(src_y + y) * static_cast<size_t>(src_w) + static_cast<size_t>(src_x)) * 3u;
            const size_t dst_row = (static_cast<size_t>(dst_y + y) * static_cast<size_t>(total_width) + static_cast<size_t>(dst_x)) * 3u;
            const size_t copy_bytes = static_cast<size_t>(inter_w) * 3u;
            if (src_row + copy_bytes > win.rgb.size() || dst_row + copy_bytes > merged.size()) continue;
            std::memcpy(merged.data() + dst_row, win.rgb.data() + src_row, copy_bytes);
        }
    }

    outcome.frame = std::move(merged);
    outcome.ok = true;
    return outcome;
}

CaptureGrabOutcome compose_linear(const std::vector<ProcessUiWindowTile>& tiles,
                                  int padding_px,
                                  ProcessUiCompositeLayout layout,
                                  int grid_cols)
{
    CaptureGrabOutcome outcome;
    if (tiles.empty()) return outcome;

    std::vector<ProcessUiWindowTile> ordered = tiles;
    std::sort(ordered.begin(), ordered.end(), [](const ProcessUiWindowTile& a, const ProcessUiWindowTile& b) {
        return a.z_order < b.z_order;
    });

    const int pad = (std::max)(0, padding_px);
    int total_w = 0;
    int total_h = 0;

    if (layout == ProcessUiCompositeLayout::Horizontal) {
        for (size_t i = 0; i < ordered.size(); ++i) {
            total_w += ordered[i].w + (i + 1 < ordered.size() ? pad : 0);
            total_h = (std::max)(total_h, ordered[i].h);
        }
    } else if (layout == ProcessUiCompositeLayout::Vertical) {
        for (size_t i = 0; i < ordered.size(); ++i) {
            total_h += ordered[i].h + (i + 1 < ordered.size() ? pad : 0);
            total_w = (std::max)(total_w, ordered[i].w);
        }
    } else {
        const int cols = (std::max)(1, grid_cols);
        const int n = static_cast<int>(ordered.size());
        const int rows = (n + cols - 1) / cols;
        std::vector<int> col_w(static_cast<size_t>(cols), 0);
        std::vector<int> row_h(static_cast<size_t>(rows), 0);
        for (int i = 0; i < n; ++i) {
            const int r = i / cols;
            const int c = i % cols;
            const ProcessUiWindowTile& t = ordered[static_cast<size_t>(i)];
            col_w[static_cast<size_t>(c)] = (std::max)(col_w[static_cast<size_t>(c)], t.w);
            row_h[static_cast<size_t>(r)] = (std::max)(row_h[static_cast<size_t>(r)], t.h);
        }
        for (int c = 0; c < cols; ++c) total_w += col_w[static_cast<size_t>(c)];
        total_w += pad * ((std::max)(0, cols - 1));
        for (int r = 0; r < rows; ++r) total_h += row_h[static_cast<size_t>(r)];
        total_h += pad * ((std::max)(0, rows - 1));
    }

    make_even(total_w);
    make_even(total_h);
    if (total_w <= 0 || total_h <= 0) return outcome;

    std::vector<uint8_t> merged(static_cast<size_t>(total_w) * static_cast<size_t>(total_h) * 3u, 0);

    auto blit_at = [&](const ProcessUiWindowTile& win, int dst_x, int dst_y) {
        for (int y = 0; y < win.h; ++y) {
            for (int x = 0; x < win.w; ++x) {
                const int dx = dst_x + x;
                const int dy = dst_y + y;
                if (dx < 0 || dy < 0 || dx >= total_w || dy >= total_h) continue;
                const size_t src_i = (static_cast<size_t>(y) * static_cast<size_t>(win.w) + static_cast<size_t>(x)) * 3u;
                const size_t dst_i = (static_cast<size_t>(dy) * static_cast<size_t>(total_w) + static_cast<size_t>(dx)) * 3u;
                merged[dst_i + 0] = win.rgb[src_i + 0];
                merged[dst_i + 1] = win.rgb[src_i + 1];
                merged[dst_i + 2] = win.rgb[src_i + 2];
            }
        }
    };

    if (layout == ProcessUiCompositeLayout::Horizontal) {
        int x = 0;
        for (size_t i = 0; i < ordered.size(); ++i) {
            const ProcessUiWindowTile& t = ordered[i];
            blit_at(t, x, (total_h - t.h) / 2);
            x += t.w + (i + 1 < ordered.size() ? pad : 0);
        }
    } else if (layout == ProcessUiCompositeLayout::Vertical) {
        int y = 0;
        for (size_t i = 0; i < ordered.size(); ++i) {
            const ProcessUiWindowTile& t = ordered[i];
            blit_at(t, (total_w - t.w) / 2, y);
            y += t.h + (i + 1 < ordered.size() ? pad : 0);
        }
    } else {
        const int cols = (std::max)(1, grid_cols);
        const int n = static_cast<int>(ordered.size());
        const int rows = (n + cols - 1) / cols;
        std::vector<int> col_w(static_cast<size_t>(cols), 0);
        std::vector<int> row_h(static_cast<size_t>(rows), 0);
        for (int i = 0; i < n; ++i) {
            const int r = i / cols;
            const int c = i % cols;
            const ProcessUiWindowTile& t = ordered[static_cast<size_t>(i)];
            col_w[static_cast<size_t>(c)] = (std::max)(col_w[static_cast<size_t>(c)], t.w);
            row_h[static_cast<size_t>(r)] = (std::max)(row_h[static_cast<size_t>(r)], t.h);
        }
        std::vector<int> col_x(static_cast<size_t>(cols), 0);
        for (int c = 1; c < cols; ++c) {
            col_x[static_cast<size_t>(c)] =
                col_x[static_cast<size_t>(c - 1)] + col_w[static_cast<size_t>(c - 1)] + pad;
        }
        std::vector<int> row_y(static_cast<size_t>(rows), 0);
        for (int r = 1; r < rows; ++r) {
            row_y[static_cast<size_t>(r)] =
                row_y[static_cast<size_t>(r - 1)] + row_h[static_cast<size_t>(r - 1)] + pad;
        }
        for (int i = 0; i < n; ++i) {
            const int r = i / cols;
            const int c = i % cols;
            const ProcessUiWindowTile& t = ordered[static_cast<size_t>(i)];
            const int dx = col_x[static_cast<size_t>(c)];
            const int dy = row_y[static_cast<size_t>(r)];
            blit_at(t, dx, dy);
        }
    }

    outcome.frame = std::move(merged);
    outcome.width = total_w;
    outcome.height = total_h;
    outcome.cap_min_left = 0;
    outcome.cap_min_top = 0;
    outcome.ok = true;
    if (layout != ProcessUiCompositeLayout::Bbox) {
        static auto s_last = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (now - s_last > std::chrono::seconds(5)) {
            s_last = now;
            std::cout << "[capture] composite layout is not bbox: mouse mapping uses synthetic canvas (0,0)-(" << total_w
                      << "," << total_h << ")\n";
        }
    }
    return outcome;
}

CaptureGrabOutcome compose(const std::vector<ProcessUiWindowTile>& tiles, ProcessUiCompositeLayout layout,  int padding_px, int grid_cols)
{
    if (tiles.size() == 1) {
        CaptureGrabOutcome o;
        const ProcessUiWindowTile& t = tiles[0];
        o.frame = t.rgb;
        o.width = t.w;
        o.height = t.h;
        o.cap_min_left = t.origin_left;
        o.cap_min_top = t.origin_top;
        o.ok = true;
        return o;
    }
    if (layout == ProcessUiCompositeLayout::Bbox) return compose_bbox(tiles);
    return compose_linear(tiles, padding_px, layout, grid_cols);
}

} // namespace

ProcessUiCaptureOptions ProcessUiCapture::load_layout_options_from_config()
{
    ProcessUiCaptureOptions o;
    std::string lay = to_lower_ascii(runtime_config::get_string("RPC_COMPOSITE_LAYOUT", "bbox"));
    if (lay == "horizontal" || lay == "h") o.composite_layout = ProcessUiCompositeLayout::Horizontal;
    else if (lay == "vertical" || lay == "v") o.composite_layout = ProcessUiCompositeLayout::Vertical;
    else if (lay == "grid" || lay == "g") o.composite_layout = ProcessUiCompositeLayout::Grid;
    else o.composite_layout = ProcessUiCompositeLayout::Bbox;

    o.composite_padding_px = (std::max)(0, runtime_config::get_int("RPC_COMPOSITE_PADDING", 8));
    o.composite_grid_columns = (std::max)(1, runtime_config::get_int("RPC_COMPOSITE_GRID_COLS", 2));
    return o;
}

CaptureGrabOutcome ProcessUiCapture::grab_process_ui_rgb(const std::vector<window_ops::window_info>& surfaces, const ProcessUiCaptureOptions& options, ICaptureSource& capture, uint64_t now_unix_ms)
{
    CaptureGrabOutcome outcome;
    outcome.ok = false;
    outcome.need_hold_on_empty_fallback = false;

    if (surfaces.empty())
    {
		std::cout << "[capture] no surfaces to capture\n";
        return outcome;
    }

    std::vector<ProcessUiWindowTile> tiles;
    if (!capture.capture_tiles(surfaces, tiles, now_unix_ms)) {
		std::cout << "[capture] capture_tiles failed for " << tiles.size() << " tiles\n";
        return outcome;
    }

    outcome = compose(tiles, options.composite_layout, options.composite_padding_px, options.composite_grid_columns);
    if (!outcome.ok || outcome.frame.empty() || outcome.width <= 0 || outcome.height <= 0) {
        outcome.ok = false;
        outcome.frame.clear();
		std::cout << "[capture] compose failed for " << tiles.size() << " tiles\n";
    }
    return outcome;
}

bool ProcessUiCapture::grab_process_ui_raw_frame(const std::vector<window_ops::window_info>& surfaces, ICaptureSource& capture, uint64_t now_unix_ms, uint64_t prep_unix_ms, uint64_t frame_id,  rpc_video_contract::RawFrame& out_frame, rpc_video_contract::TelemetrySnapshot& out_telem)
{
    ProcessUiCaptureOptions options = load_layout_options_from_config();
    out_frame = rpc_video_contract::RawFrame{};
    out_telem = rpc_video_contract::TelemetrySnapshot{};

    // Keep telemetry valid even when capture fails.
    out_telem.frame_unix_ms = static_cast<rpc_video_contract::TimeUs>(now_unix_ms);
    out_telem.prep_unix_ms = static_cast<rpc_video_contract::TimeUs>(prep_unix_ms);
    out_telem.backend = capture.backend();
    out_telem.frame_id = frame_id;

    // Reuse existing implementation for now.
    CaptureGrabOutcome o = grab_process_ui_rgb(surfaces, options, capture, now_unix_ms);
    if (!o.ok || o.frame.empty() || o.width <= 0 || o.height <= 0)  return false;

    // 步骤 1：读取是否启用「可疑帧/黑帧」过滤（可由 RPC_FILTER_CAPTURE_BLACK_FRAMES 关闭）。
    const bool filter_black = runtime_config::get_bool("RPC_FILTER_CAPTURE_BLACK_FRAMES", true);
    // 1a：关闭过滤则一律放行,整帧可疑（如全黑启动画面）则丢弃，
    if (filter_black && capture_rgb::is_suspicious_capture_frame(o.frame, o.width, o.height)) {
        std::cout << "[capture] discard suspicious frame before first video (e.g. black startup)\n";
        return false;
    }

    const uint64_t cap_done_unix_ms = rpc_unix_epoch_ms();

    out_telem.capture_size = rpc_video_contract::VideoSize{ o.width, o.height };
    out_telem.capture_unix_ms = static_cast<rpc_video_contract::TimeUs>(cap_done_unix_ms);
    out_telem.encode_unix_ms = 0;

    out_frame.frame_id = frame_id;
    out_frame.pts_us = static_cast<rpc_video_contract::TimeUs>(cap_done_unix_ms) * 1000;
    out_frame.dts_us = out_frame.pts_us;
    out_frame.coded_size = rpc_video_contract::VideoSize{ o.width, o.height };
    out_frame.visible_rect = rpc_video_contract::VideoRect{ o.cap_min_left, o.cap_min_top, o.width, o.height };
    out_frame.display_size = rpc_video_contract::VideoSize{};
    out_frame.format = rpc_video_contract::PixelFormat::RGB24;
    out_frame.rotation = rpc_video_contract::Rotation::R0;
    out_frame.is_screen_content = true;
    out_frame.storage = rpc_video_contract::FrameStorageKind::Cpu;

    // Move RGB buffer into contract-owned heap vector (zero-copy on the data).
    auto* vec = new std::vector<uint8_t>(std::move(o.frame));
    out_frame.owned.bytes.data = vec->data();
    out_frame.owned.bytes.size = static_cast<uint32_t>(vec->size());
    out_frame.owned.opaque = vec;
    out_frame.owned.release = &release_vector_u8;

    out_frame.plane_count = 1;
    out_frame.planes[0].data = vec->data();
    out_frame.planes[0].stride_bytes = o.width * 3;
    out_frame.planes[0].size_bytes = static_cast<uint32_t>(vec->size());

    return true;
}
