// ============================================================
// frame_composer.cpp
// ============================================================

#include "frame_composer.h"
#include "app/runtime_config.h"
#include "common/character_conversion.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <iostream>
#include <numeric>
#include <chrono>

#if defined(_WIN32)
#  include <windows.h>   // IntersectRect
#endif

namespace capture {

// ---- 配置读取 -----------------------------------------------

/*static*/
FrameComposer::Options FrameComposer::options_from_config() {
    Options o;
    const std::string lay =
        to_lower_ascii(runtime_config::get_string("RPC_COMPOSITE_LAYOUT", "bbox"));
    if      (lay == "horizontal" || lay == "h") o.layout = CompositeLayout::Horizontal;
    else if (lay == "vertical"   || lay == "v") o.layout = CompositeLayout::Vertical;
    else if (lay == "grid"       || lay == "g") o.layout = CompositeLayout::Grid;
    else                                         o.layout = CompositeLayout::Bbox;

    o.padding_px   = max(0, runtime_config::get_int("RPC_COMPOSITE_PADDING",    8));
    o.grid_columns = max(1, runtime_config::get_int("RPC_COMPOSITE_GRID_COLS",  2));
    return o;
}

// ---- 主接口 -------------------------------------------------

ComposedFrame FrameComposer::compose(std::span<const WindowTile> tiles,
                                      const Options&              opts) const {
    if (tiles.empty()) return {};

    // 单瓦片：直接包装，零拷贝构造
    if (tiles.size() == 1) {
        const WindowTile& t = tiles[0];
        ComposedFrame f;
        f.ok          = true;
        f.width       = t.w;
        f.height      = t.h;
        f.origin_left = t.origin_left;
        f.origin_top  = t.origin_top;
        f.pixels      = t.rgb;
        return f;
    }

    if (opts.layout == CompositeLayout::Bbox)
        return compose_bbox(tiles);
    return compose_linear(tiles, opts);
}

// ---- Bbox 合成 ----------------------------------------------

ComposedFrame FrameComposer::compose_bbox(std::span<const WindowTile> tiles) const {
    ComposedFrame out;
    if (tiles.empty()) return out;

    // 计算联合 bbox
    int min_l = INT_MAX, min_t = INT_MAX;
    int max_r = INT_MIN, max_b = INT_MIN;
    for (const auto& t : tiles) {
        min_l = min(min_l, t.origin_left);
        min_t = min(min_t, t.origin_top);
        max_r = max(max_r, t.origin_left + t.w);
        max_b = max(max_b, t.origin_top  + t.h);
    }

    int total_w = max_r - min_l;
    int total_h = max_b - min_t;
    make_even(total_w);
    make_even(total_h);
    if (total_w <= 0 || total_h <= 0) return out;

    out.pixels.assign(static_cast<size_t>(total_w) * static_cast<size_t>(total_h) * 3u, 0);

    // 按 z_order 排序（z 越小越靠前，绘制时先绘背景）
    std::vector<size_t> order(tiles.size());
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return tiles[a].z_order < tiles[b].z_order;
    });

    const RECT clip{ min_l, min_t, min_l + total_w, min_t + total_h };

    // 反向迭代：z 值大的先画（背景），z 值小的后画（前景）
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const WindowTile& win = tiles[*it];
        const RECT win_rect{ win.origin_left,
                              win.origin_top,
                              win.origin_left + win.w,
                              win.origin_top  + win.h };
#if defined(_WIN32)
        RECT inter{};
        if (!IntersectRect(&inter, &win_rect, &clip)) continue;
#else
        // 非 Windows 下手动计算交集
        RECT inter{
            max(win_rect.left,  clip.left),
            max(win_rect.top,   clip.top),
            min(win_rect.right, clip.right),
            min(win_rect.bottom,clip.bottom)
        };
        if (inter.left >= inter.right || inter.top >= inter.bottom) continue;
#endif
        const int src_x    = inter.left - win.origin_left;
        const int src_y    = inter.top  - win.origin_top;
        const int dst_x    = inter.left - min_l;
        const int dst_y    = inter.top  - min_t;
        const int inter_w  = inter.right  - inter.left;
        const int inter_h  = inter.bottom - inter.top;
        if (inter_w <= 0 || inter_h <= 0 || win.w <= 0) continue;

        for (int y = 0; y < inter_h; ++y) {
            const size_t src_off =
                (static_cast<size_t>(src_y + y) * static_cast<size_t>(win.w)
                 + static_cast<size_t>(src_x)) * 3u;
            const size_t dst_off =
                (static_cast<size_t>(dst_y + y) * static_cast<size_t>(total_w)
                 + static_cast<size_t>(dst_x)) * 3u;
            const size_t copy_bytes = static_cast<size_t>(inter_w) * 3u;
            if (src_off + copy_bytes > win.rgb.size()) continue;
            if (dst_off + copy_bytes > out.pixels.size()) continue;
            std::memcpy(out.pixels.data() + dst_off,
                        win.rgb.data()    + src_off, copy_bytes);
        }
    }

    out.ok          = true;
    out.width       = total_w;
    out.height      = total_h;
    out.origin_left = min_l;
    out.origin_top  = min_t;
    return out;
}

// ---- Linear / Grid 合成 ------------------------------------

ComposedFrame FrameComposer::compose_linear(std::span<const WindowTile> tiles,
                                             const Options&              opts) const {
    ComposedFrame out;
    if (tiles.empty()) return out;

    // 按 z_order 排序
    std::vector<size_t> order(tiles.size());
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return tiles[a].z_order < tiles[b].z_order;
    });

    const int pad  = max(0, opts.padding_px);
    const int n    = static_cast<int>(order.size());
    int total_w = 0, total_h = 0;

    // ---- 计算总尺寸 -----------------------------------------
    if (opts.layout == CompositeLayout::Horizontal) {
        for (int i = 0; i < n; ++i) {
            total_w += tiles[order[static_cast<size_t>(i)]].w
                     + (i + 1 < n ? pad : 0);
            total_h  = max(total_h, tiles[order[static_cast<size_t>(i)]].h);
        }
    } else if (opts.layout == CompositeLayout::Vertical) {
        for (int i = 0; i < n; ++i) {
            total_h += tiles[order[static_cast<size_t>(i)]].h
                     + (i + 1 < n ? pad : 0);
            total_w  = max(total_w, tiles[order[static_cast<size_t>(i)]].w);
        }
    } else {
        // Grid
        const int cols = opts.grid_columns;
        const int rows = (n + cols - 1) / cols;
        std::vector<int> col_w(static_cast<size_t>(cols), 0);
        std::vector<int> row_h(static_cast<size_t>(rows), 0);
        for (int i = 0; i < n; ++i) {
            const int r = i / cols, c = i % cols;
            const WindowTile& t = tiles[order[static_cast<size_t>(i)]];
            col_w[static_cast<size_t>(c)] = max(col_w[static_cast<size_t>(c)], t.w);
            row_h[static_cast<size_t>(r)] = max(row_h[static_cast<size_t>(r)], t.h);
        }
        for (int c = 0; c < cols; ++c) total_w += col_w[static_cast<size_t>(c)];
        for (int r = 0; r < rows; ++r) total_h += row_h[static_cast<size_t>(r)];
        total_w += pad * max(0, cols - 1);
        total_h += pad * max(0, rows - 1);
    }

    make_even(total_w);
    make_even(total_h);
    if (total_w <= 0 || total_h <= 0) return out;

    out.pixels.assign(static_cast<size_t>(total_w) * static_cast<size_t>(total_h) * 3u, 0);

    // ---- blit 辅助 ------------------------------------------
    auto blit_at = [&](const WindowTile& win, int dst_x, int dst_y) {
        for (int y = 0; y < win.h; ++y) {
            for (int x = 0; x < win.w; ++x) {
                const int dx = dst_x + x, dy = dst_y + y;
                if (dx < 0 || dy < 0 || dx >= total_w || dy >= total_h) continue;
                const size_t si =
                    (static_cast<size_t>(y) * static_cast<size_t>(win.w)
                     + static_cast<size_t>(x)) * 3u;
                const size_t di =
                    (static_cast<size_t>(dy) * static_cast<size_t>(total_w)
                     + static_cast<size_t>(dx)) * 3u;
                out.pixels[di+0] = win.rgb[si+0];
                out.pixels[di+1] = win.rgb[si+1];
                out.pixels[di+2] = win.rgb[si+2];
            }
        }
    };

    // ---- 绘制 -----------------------------------------------
    if (opts.layout == CompositeLayout::Horizontal) {
        int x = 0;
        for (int i = 0; i < n; ++i) {
            const WindowTile& t = tiles[order[static_cast<size_t>(i)]];
            blit_at(t, x, (total_h - t.h) / 2);
            x += t.w + (i + 1 < n ? pad : 0);
        }
    } else if (opts.layout == CompositeLayout::Vertical) {
        int y = 0;
        for (int i = 0; i < n; ++i) {
            const WindowTile& t = tiles[order[static_cast<size_t>(i)]];
            blit_at(t, (total_w - t.w) / 2, y);
            y += t.h + (i + 1 < n ? pad : 0);
        }
    } else {
        // Grid
        const int cols = opts.grid_columns;
        const int rows = (n + cols - 1) / cols;
        std::vector<int> col_w(static_cast<size_t>(cols), 0);
        std::vector<int> row_h(static_cast<size_t>(rows), 0);
        for (int i = 0; i < n; ++i) {
            const int r = i / cols, c = i % cols;
            const WindowTile& t = tiles[order[static_cast<size_t>(i)]];
            col_w[static_cast<size_t>(c)] = max(col_w[static_cast<size_t>(c)], t.w);
            row_h[static_cast<size_t>(r)] = max(row_h[static_cast<size_t>(r)], t.h);
        }
        std::vector<int> col_x(static_cast<size_t>(cols), 0);
        std::vector<int> row_y(static_cast<size_t>(rows), 0);
        for (int c = 1; c < cols; ++c)
            col_x[static_cast<size_t>(c)] =
                col_x[static_cast<size_t>(c-1)] + col_w[static_cast<size_t>(c-1)] + pad;
        for (int r = 1; r < rows; ++r)
            row_y[static_cast<size_t>(r)] =
                row_y[static_cast<size_t>(r-1)] + row_h[static_cast<size_t>(r-1)] + pad;
        for (int i = 0; i < n; ++i) {
            const int r = i / cols, c = i % cols;
            blit_at(tiles[order[static_cast<size_t>(i)]],
                    col_x[static_cast<size_t>(c)],
                    row_y[static_cast<size_t>(r)]);
        }
    }

    // 非 Bbox 模式：鼠标坐标映射使用合成画布 (0,0)
    static auto s_last = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now - s_last > std::chrono::seconds(5)) {
        s_last = now;
        std::cout << "[composer] non-bbox layout: canvas origin=(0,0) size=("
                  << total_w << "," << total_h << ")\n";
    }

    out.ok          = true;
    out.width       = total_w;
    out.height      = total_h;
    out.origin_left = 0;
    out.origin_top  = 0;
    return out;
}

} // namespace capture
