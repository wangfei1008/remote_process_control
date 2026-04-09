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

    // 入口策略：
    // - use_hw_capture=true 走 DXGI（硬件/桌面复制）优先；失败时可按 lock_capture_backend 决定是否回退 GDI。
    // - capture_all_windows=true 时，为了“采集稳定与可控”，统一走 GDI 的窗口枚举/拼图逻辑（DXGI 路径仅针对单窗口）。
    if (use_hw_capture) {
        if (capture_all_windows) {
            outcome.used_hw_capture = false;
            outcome.frame = WindowFrameGrabber::capture_all_windows_image(
                gdi_capture, capture_pid, main_window, 1024,
                outcome.width, outcome.height, outcome.cap_min_left, outcome.cap_min_top);
            return outcome;
        }

        // 1) DXGI 采集主窗口：成功则直接返回
        outcome.frame = dxgi_capture.capture_window_rgb(main_window, outcome.width, outcome.height, outcome.cap_min_left, outcome.cap_min_top);
        if (!outcome.frame.empty()) {
            outcome.used_hw_capture = true;
            capture_backend_state.on_dxgi_success();
            return outcome;
        }

        // 2) DXGI 返回空帧：交给状态机判断是否需要 reset duplication
        bool should_reset_duplication = false;
        capture_backend_state.on_dxgi_empty(rpc_unix_epoch_ms(), should_reset_duplication);
        if (should_reset_duplication) {
            dxgi_capture.reset();
            std::cout << "[capture] dxgi repeated empty, reset duplication\n";
        }

        // DXGI 空帧可能持续出现（最小化/遮挡/DWM/权限/特殊渲染等），这里做节流打印，避免刷屏。
        static auto s_last_dxgi_fail_log = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - s_last_dxgi_fail_log > std::chrono::seconds(1)) {
            s_last_dxgi_fail_log = now;
            std::cout << "[capture] dxgi returned empty\n";
        }

        // 3) 是否允许回退：
        // - lock_capture_backend=true：坚持 DXGI（由上层状态机决定何时切换），这里不主动回退，避免“前端看似恢复但输入映射/采集目标抖动”。
        // - lock_capture_backend=false：允许使用 GDI 作为兜底（但仍不自动升级到 all-windows，避免误采集无关窗口）。
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

    // use_hw_capture=false：明确走 GDI。
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

