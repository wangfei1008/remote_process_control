#include "capture/frame_sanitizer.h"

#include "capture/capture_rgb_heuristics.h"
#include "common/rpc_time.h"
#include "capture/window_frame_grabber.h"

#include <chrono>
#include <iostream>

bool FrameSanitizer::sanitize_frame(std::vector<uint8_t>& frame,
                                    int& width,
                                    int& height,
                                    int& cap_min_left,
                                    int& cap_min_top,
                                    bool had_successful_video,
                                    bool have_last_good_sample,
                                    bool hw_capture_active,
                                    bool capture_all_windows,
                                    HWND main_window,
                                    const std::vector<uint8_t>& last_good_rgb_frame,
                                    int last_good_rgb_w,
                                    int last_good_rgb_h,
                                    GdiCapture& gdi_capture,
                                    CaptureBackendState& capture_backend_state)
{
    // 启动首帧阶段不做“可疑帧修复”：
    // 1) 避免在冷启动时增加额外判定开销；
    // 2) 没有 last_good_rgb 时，修复也无从谈起。
    if (!(had_successful_video && have_last_good_sample)) return true;

    // “只允许一种采集方式”约束下，不再使用 GDI 二次重采逻辑；
    // 这些参数保留在接口中以避免大范围签名变更。
    (void)capture_all_windows;
    (void)main_window;
    (void)gdi_capture;

    // 这两步是轻量启发式采样（抽样点有限），常态开销较小；
    // 主要用于快速识别黑帧/低方差/顶黑条等“疑似坏帧”。
    const bool has_top_black_strip = capture_rgb::frame_has_top_black_strip_rgb24(frame, width, height);
    bool suspicious = capture_rgb::is_suspicious_capture_frame(frame, width, height);

    if (has_top_black_strip) {
        const bool can_repair_with_prev = (!last_good_rgb_frame.empty() && last_good_rgb_w == width && last_good_rgb_h == height && last_good_rgb_frame.size() == frame.size());
        if (can_repair_with_prev) {
            // 低成本修补：仅替换顶部区域，不触发额外抓屏。
            capture_rgb::repair_top_strip_from_previous(frame, width, height, last_good_rgb_frame);
            suspicious = capture_rgb::is_suspicious_capture_frame(frame, width, height);
        }
        // 记录不稳定信号，供后续后端状态机决定是否降级采集后端。
        capture_backend_state.on_top_black_strip_detected(rpc_unix_epoch_ms(), hw_capture_active);
    } else {
        capture_backend_state.on_no_top_black_strip();
    }

    // 为了满足“从头到尾只允许一种采集方式”的约束：
    // 不再在 suspicious 时切换到 GDI 做二次重采。
    // 让上层按 suspicious 结果丢帧/hold，而不是混用两套尺寸源。

    return !suspicious;
}

