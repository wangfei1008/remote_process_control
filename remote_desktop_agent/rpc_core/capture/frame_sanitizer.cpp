#include "capture/frame_sanitizer.h"

#include "app/runtime_config.h"
#include "capture/capture_rgb_heuristics.h"

#include <chrono>
#include <iostream>

bool FrameSanitizer::sanitize_frame(std::vector<uint8_t>& frame,
                                    int& width,
                                    int& height,
                                    bool had_successful_video,
                                    bool have_last_good_sample,
                                    const std::vector<uint8_t>& last_good_rgb_frame,
                                    int last_good_rgb_w,
                                    int last_good_rgb_h)
{
    // 步骤 1：读取是否启用「可疑帧/黑帧」过滤（可由 RPC_FILTER_CAPTURE_BLACK_FRAMES 关闭）。
    const bool filter_black = runtime_config::get_bool("RPC_FILTER_CAPTURE_BLACK_FRAMES", true);
    // 1a：关闭过滤则一律放行。
    if (!filter_black) return true;

    // 步骤 2：尚未出现过成功视频、或尚无上一帧参考时，只做「启动期」粗过滤。
    if (!(had_successful_video && have_last_good_sample)) {
        // 2a：空帧或尺寸非法视为无可判定，放行（由上游 discard 逻辑处理）。
        if (frame.empty() || width <= 0 || height <= 0) return true;
        // 2b：整帧可疑（如全黑启动画面）则丢弃，并节流打印日志。
        if (capture_rgb::is_suspicious_capture_frame(frame, width, height)) {
            static auto s_last_log = std::chrono::steady_clock::time_point{};
            const auto now = std::chrono::steady_clock::now();
            if (now - s_last_log > std::chrono::seconds(2)) {
                s_last_log = now;
                std::cout << "[capture] discard suspicious frame before first video (e.g. black startup)\n";
            }
            return false;
        }
        return true;
    }

    // 步骤 3：检测顶黑条 heuristic，并对当前帧做「可疑」判定（全黑/异常等）。
    const bool has_top_black_strip = capture_rgb::frame_has_top_black_strip_rgb24(frame, width, height);
    bool suspicious = capture_rgb::is_suspicious_capture_frame(frame, width, height);

    // 步骤 4：顶黑条分支——可则用上一好帧修补顶条后重算可疑性（不触发采集方式切换）。
    if (has_top_black_strip) {
        const bool can_repair_with_prev = (!last_good_rgb_frame.empty() && last_good_rgb_w == width && last_good_rgb_h == height &&
                                           last_good_rgb_frame.size() == frame.size());
        if (can_repair_with_prev) {
            capture_rgb::repair_top_strip_from_previous(frame, width, height, last_good_rgb_frame);
            suspicious = capture_rgb::is_suspicious_capture_frame(frame, width, height);
        }
    }

    // 步骤 5：修补后若仍判定为可疑帧则丢弃（返回 false），否则保留（返回 true）。
    return !suspicious;
}
