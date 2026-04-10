#include "capture/process_ui_capture.h"

#include "app/runtime_config.h"
#include "capture/capture_backend_state.h"
#include "capture/dxgi_capture.h"
#include "capture/gdi_capture.h"
#include "capture/process_surface_enumerator.h"
#include "capture/window_frame_grabber.h"
#include "capture/window_visibility_diagnostics.h"
#include "common/rpc_time.h"
#include "common/window_rect_utils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>

namespace {

const char* layout_name(ProcessUiCompositeLayout layout)
{
    switch (layout) {
        case ProcessUiCompositeLayout::Horizontal: return "horizontal";
        case ProcessUiCompositeLayout::Vertical: return "vertical";
        case ProcessUiCompositeLayout::Grid: return "grid";
        case ProcessUiCompositeLayout::Bbox:
        default: return "bbox";
    }
}

std::string truncate_for_log(const std::string& s, size_t max_chars)
{
    if (s.size() <= max_chars) return s;
    return s.substr(0, max_chars) + "...";
}

struct WindowTile {
    HWND hwnd = nullptr;
    RECT rect_screen{};
    int z_order = 0;
    std::vector<uint8_t> rgb;
    int w = 0;
    int h = 0;
    int origin_left = 0;
    int origin_top = 0;
};

void maybe_log_process_ui_capture(DWORD pid,
                                  const ProcessUiCaptureOptions& options,
                                  const std::vector<ProcessSurfaceInfo>& surfaces,
                                  const std::vector<WindowTile>& tiles,
                                  const CaptureGrabOutcome& outcome)
{
    if (!runtime_config::get_bool("RPC_LOG_PROCESS_SURFACES", false)) return;

    const int interval_ms =
        (std::max)(100, runtime_config::get_int("RPC_LOG_PROCESS_SURFACES_INTERVAL_MS", 1000));
    static auto s_last_log = std::chrono::steady_clock::time_point{};
    const auto now_steady = std::chrono::steady_clock::now();
    if (now_steady - s_last_log < std::chrono::milliseconds(interval_ms)) return;
    s_last_log = now_steady;

    const char* be =
        options.session_backend == ProcessUiSessionBackendMode::Dxgi ? "dxgi" : "gdi";

    int union_l = INT_MAX;
    int union_t = INT_MAX;
    int union_r = INT_MIN;
    int union_b = INT_MIN;
    for (const auto& s : surfaces) {
        union_l = (std::min)(union_l, static_cast<int>(s.rect_screen.left));
        union_t = (std::min)(union_t, static_cast<int>(s.rect_screen.top));
        union_r = (std::max)(union_r, static_cast<int>(s.rect_screen.right));
        union_b = (std::max)(union_b, static_cast<int>(s.rect_screen.bottom));
    }

    std::cout << "[proc_ui] pid=" << pid << " backend=" << be << " layout=" << layout_name(options.composite_layout)
              << " surfaces=" << surfaces.size() << std::endl;
    std::cout << "[proc_ui] enum_union_rect LTRB=(" << union_l << "," << union_t << "," << union_r << "," << union_b
              << ") size=" << (union_r - union_l) << "x" << (union_b - union_t) << std::endl;

    for (size_t i = 0; i < surfaces.size(); ++i) {
        const auto& s = surfaces[i];
        const RECT& r = s.rect_screen;
        std::cout << "[proc_ui]  z=" << s.z_order << " hwnd=0x" << static_cast<void*>(s.hwnd) << " \""
                  << truncate_for_log(s.title, 72) << "\" class=\"" << truncate_for_log(s.class_name, 48)
                  << "\" rect=(" << r.left << "," << r.top << "," << r.right << "," << r.bottom << ") size="
                  << (r.right - r.left) << "x" << (r.bottom - r.top) << std::endl;

        const WindowVisibilityDiagnosis vis = diagnose_window_visibility(s.hwnd);
        std::cout << "[proc_ui]  vis z=" << s.z_order << " IsWindowVisible=" << (vis.is_visible_api ? 1 : 0)
                  << " iconic=" << (vis.minimized ? 1 : 0) << " style=0x" << std::hex
                  << static_cast<uintptr_t>(static_cast<ULONG_PTR>(vis.style)) << " exstyle=0x"
                  << static_cast<uintptr_t>(static_cast<ULONG_PTR>(vis.ex_style)) << std::dec << " layered="
                  << (vis.layered ? 1 : 0)
                  << " lwa_ok=" << (vis.layered_attrs_valid ? 1 : 0) << " lwa_alpha=" << static_cast<int>(vis.layered_alpha)
                  << " lwa_flags=0x" << std::hex << vis.layered_flags << std::dec << " cloak=0x" << std::hex
                  << vis.cloaked << std::dec << " disp_aff=0x" << std::hex << vis.display_affinity << std::dec
                  << " owner=" << (vis.has_owner ? 1 : 0) << " ex_transp=" << (vis.ex_transparent ? 1 : 0)
                  << " toolwin=" << (vis.ex_tool_window ? 1 : 0) << " noact=" << (vis.ex_no_activate ? 1 : 0)
                  << " client=" << vis.client_w << "x" << vis.client_h << std::endl;
        std::cout << "[proc_ui]  vis z=" << s.z_order << " why=" << vis.reason_summary << std::endl;
    }

    if (tiles.size() == surfaces.size()) {
        for (size_t i = 0; i < tiles.size(); ++i) {
            const auto& t = tiles[i];
            std::cout << "[proc_ui]  cap z=" << t.z_order << " origin=(" << t.origin_left << "," << t.origin_top
                      << ") wh=" << t.w << "x" << t.h << std::endl;
        }
    }

    std::cout << "[proc_ui] outcome ok=" << (outcome.ok ? 1 : 0) << " cap_rect=(" << outcome.cap_min_left << ","
              << outcome.cap_min_top << ")-(" << (outcome.cap_min_left + outcome.width) << ","
              << (outcome.cap_min_top + outcome.height) << ") wh=" << outcome.width << "x" << outcome.height
              << std::endl;
}

bool capture_tile_gdi(GdiCapture& gdi, const ProcessSurfaceInfo& surf, WindowTile& out)
{
    out = WindowTile{};
    out.hwnd = surf.hwnd;
    out.rect_screen = surf.rect_screen;
    out.z_order = surf.z_order;
    int cap_l = 0;
    int cap_t = 0;
    out.rgb = WindowFrameGrabber::capture_main_window_image(
        gdi, surf.hwnd, out.w, out.h, cap_l, cap_t, true);
    if (out.rgb.empty() || out.w <= 0 || out.h <= 0) return false;
    const size_t expected = static_cast<size_t>(out.w) * static_cast<size_t>(out.h) * 3u;
    if (out.rgb.size() != expected) return false;
    out.origin_left = cap_l;
    out.origin_top = cap_t;
    return true;
}

bool capture_all_tiles_gdi(GdiCapture& gdi, const std::vector<ProcessSurfaceInfo>& surfaces, std::vector<WindowTile>& tiles)
{
    tiles.clear();
    tiles.reserve(surfaces.size());
    for (const auto& s : surfaces) {
        WindowTile t;
        if (!capture_tile_gdi(gdi, s, t)) return false;
        tiles.push_back(std::move(t));
    }
    return !tiles.empty();
}

bool capture_all_tiles_dxgi(DXGICapture& dxgi,
                            const std::vector<ProcessSurfaceInfo>& surfaces,
                            std::vector<WindowTile>& tiles,
                            CaptureBackendState& st,
                            uint64_t now_unix_ms)
{
    tiles.clear();
    if (surfaces.empty()) return false;
    std::vector<HWND> hwnds;
    hwnds.reserve(surfaces.size());
    for (const auto& s : surfaces) hwnds.push_back(s.hwnd);

    if (!dxgi.begin_multiwindow_desktop_capture(hwnds)) {
        bool should_reset = false;
        st.on_dxgi_empty(now_unix_ms, should_reset);
        if (should_reset) dxgi.reset();
        return false;
    }

    for (const auto& s : surfaces) {
        WindowTile t;
        t.hwnd = s.hwnd;
        t.rect_screen = s.rect_screen;
        t.z_order = s.z_order;
        t.rgb = dxgi.copy_acquired_window_to_rgb(s.hwnd, t.w, t.h, t.origin_left, t.origin_top);
        const size_t expected = static_cast<size_t>(t.w) * static_cast<size_t>(t.h) * 3u;
        if (t.rgb.empty() || t.w <= 0 || t.h <= 0 || t.rgb.size() != expected) {
            dxgi.end_desktop_capture();
            bool should_reset = false;
            st.on_dxgi_empty(now_unix_ms, should_reset);
            if (should_reset) dxgi.reset();
            return false;
        }
        tiles.push_back(std::move(t));
    }
    dxgi.end_desktop_capture();
    st.on_dxgi_success();
    return true;
}

void make_even(int& v)
{
    if (v > 0) v &= ~1;
}

CaptureGrabOutcome compose_bbox(const std::vector<WindowTile>& tiles)
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

    std::vector<WindowTile> ordered = tiles;
    std::sort(ordered.begin(), ordered.end(), [](const WindowTile& a, const WindowTile& b) {
        return a.z_order < b.z_order;
    });

    std::vector<uint8_t> merged(static_cast<size_t>(total_width) * static_cast<size_t>(total_height) * 3u, 0);
    for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) {
        const WindowTile& win = *it;
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

CaptureGrabOutcome compose_linear(const std::vector<WindowTile>& tiles,
                                  int padding_px,
                                  ProcessUiCompositeLayout layout,
                                  int grid_cols)
{
    CaptureGrabOutcome outcome;
    if (tiles.empty()) return outcome;

    std::vector<WindowTile> ordered = tiles;
    std::sort(ordered.begin(), ordered.end(), [](const WindowTile& a, const WindowTile& b) {
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
            const WindowTile& t = ordered[static_cast<size_t>(i)];
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

    auto blit_at = [&](const WindowTile& win, int dst_x, int dst_y) {
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
            const WindowTile& t = ordered[i];
            blit_at(t, x, (total_h - t.h) / 2);
            x += t.w + (i + 1 < ordered.size() ? pad : 0);
        }
    } else if (layout == ProcessUiCompositeLayout::Vertical) {
        int y = 0;
        for (size_t i = 0; i < ordered.size(); ++i) {
            const WindowTile& t = ordered[i];
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
            const WindowTile& t = ordered[static_cast<size_t>(i)];
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
            const WindowTile& t = ordered[static_cast<size_t>(i)];
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

CaptureGrabOutcome compose(const std::vector<WindowTile>& tiles,
                           ProcessUiCompositeLayout layout,
                           int padding_px,
                           int grid_cols)
{
    if (tiles.size() == 1) {
        CaptureGrabOutcome o;
        const WindowTile& t = tiles[0];
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

std::string ProcessUiCapture::to_lower_ascii(std::string value)
{
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

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

CaptureGrabOutcome ProcessUiCapture::grab_process_ui_rgb(DWORD pid,
                                                         const ProcessUiCaptureOptions& options,
                                                         GdiCapture& gdi_capture,
                                                         DXGICapture& dxgi_capture,
                                                         CaptureBackendState& capture_backend_state,
                                                         uint64_t now_unix_ms)
{
    (void)now_unix_ms;
    CaptureGrabOutcome outcome;
    outcome.ok = false;
    outcome.need_hold_on_empty_fallback = false;
    outcome.used_hw_capture = (options.session_backend == ProcessUiSessionBackendMode::Dxgi);

    const std::vector<ProcessSurfaceInfo> surfaces = ProcessSurfaceEnumerator::enumerate_visible_top_level(pid);
    if (surfaces.empty()) {
        return outcome;
    }

    std::vector<WindowTile> tiles;
    if (options.session_backend == ProcessUiSessionBackendMode::Dxgi) {
        if (!dxgi_capture.is_available()) return outcome;
        if (!capture_all_tiles_dxgi(dxgi_capture, surfaces, tiles, capture_backend_state, now_unix_ms)) return outcome;
    } else {
        if (!capture_all_tiles_gdi(gdi_capture, surfaces, tiles)) return outcome;
    }

    outcome = compose(tiles, options.composite_layout, options.composite_padding_px, options.composite_grid_columns);
    maybe_log_process_ui_capture(pid, options, surfaces, tiles, outcome);
    outcome.used_hw_capture = (options.session_backend == ProcessUiSessionBackendMode::Dxgi);
    if (!outcome.ok || outcome.frame.empty() || outcome.width <= 0 || outcome.height <= 0) {
        outcome.ok = false;
        outcome.frame.clear();
    }
    return outcome;
}
