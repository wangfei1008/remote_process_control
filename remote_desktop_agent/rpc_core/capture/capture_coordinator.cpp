#include "capture/capture_coordinator.h"

#include "common/rpc_time.h"
#include "capture/window_frame_grabber.h"

#include <chrono>
#include <iostream>

CaptureGrabOutcome CaptureCoordinator::grab_rgb_frame(bool use_hw_capture,
                                                      bool capture_all_windows,
                                                      bool lock_capture_backend,
                                                      DWORD capture_pid,
                                                      HWND main_window,
                                                      GdiCapture& gdi_capture,
                                                      DXGICapture& dxgi_capture,
                                                      CaptureBackendState& capture_backend_state)
{
    CaptureGrabOutcome outcome;

    if (use_hw_capture) {
        if (capture_all_windows) {
            outcome.used_hw_capture = false;
            outcome.frame = WindowFrameGrabber::capture_all_windows_image(
                gdi_capture, capture_pid, main_window, 1024,
                outcome.width, outcome.height, outcome.cap_min_left, outcome.cap_min_top);
            return outcome;
        }

        outcome.frame = dxgi_capture.capture_window_rgb(
            main_window, outcome.width, outcome.height, outcome.cap_min_left, outcome.cap_min_top);
        if (!outcome.frame.empty()) {
            outcome.used_hw_capture = true;
            capture_backend_state.on_dxgi_success();
            return outcome;
        }

        bool should_reset_duplication = false;
        capture_backend_state.on_dxgi_empty(rpc_unix_epoch_ms(), should_reset_duplication);
        if (should_reset_duplication) {
            dxgi_capture.reset();
            std::cout << "[capture] dxgi repeated empty, reset duplication\n";
        }

        static auto s_last_dxgi_fail_log = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - s_last_dxgi_fail_log > std::chrono::seconds(1)) {
            s_last_dxgi_fail_log = now;
            std::cout << "[capture] dxgi returned empty\n";
        }

        if (!lock_capture_backend) {
            outcome.frame = WindowFrameGrabber::capture_main_window_image(
                gdi_capture, main_window, outcome.width, outcome.height,
                outcome.cap_min_left, outcome.cap_min_top, true);
            outcome.used_hw_capture = false;
            if (outcome.frame.empty()) {
                // 不再自动回退到 all-windows，避免误采集无关窗口（如启动器/工具窗）。
                outcome.ok = false;
                outcome.need_hold_on_empty_fallback = true;
            }
        }
        return outcome;
    }

    if (capture_all_windows) {
        outcome.used_hw_capture = false;
        outcome.frame = WindowFrameGrabber::capture_all_windows_image(
            gdi_capture, capture_pid, main_window, 1024,
            outcome.width, outcome.height, outcome.cap_min_left, outcome.cap_min_top);
    } else {
        outcome.used_hw_capture = false;
        outcome.frame = WindowFrameGrabber::capture_main_window_image(
            gdi_capture, main_window, outcome.width, outcome.height,
            outcome.cap_min_left, outcome.cap_min_top, true);
    }
    return outcome;
}

