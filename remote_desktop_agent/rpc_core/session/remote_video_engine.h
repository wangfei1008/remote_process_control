#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <windows.h>
#include <vector>

#include "rtc/rtc.hpp"

#include "capture/bmp_dump_writer.h"
#include "session/remote_capture_telemetry.h"

class RemoteProcessSession;

class GdiCapture;
class DXGICapture;
class CaptureBackendState;
class FrameSanitizer;
class CaptureCoordinator;
class CaptureDiscardPolicy;
class RemoteProcessSession;
class VideoEncodePipeline;

// 视频引擎：从零重做的阶段性接口
// - start/stop：进程启停与远端退出检测
// - produce_next_video_sample：生产下一帧视频样本与遥测（当前阶段先输出空样本/零遥测）
// - request_force_keyframe：强制 IDR 请求（当前阶段先保留入口）
class remote_video_engine {
public:
    remote_video_engine(std::string exe_path, std::function<void()> on_remote_process_exit);
    ~remote_video_engine();

    void start();
    void stop();

    void produce_next_video_sample(rtc::binary& out_sample, remote_capture_telemetry& out_telemetry);

    void request_force_keyframe();
    HWND get_main_window() const;

private:
    void notify_remote_exit_if_needed();
    void exit_watch_loop();

    // 与 rpc_remote_client::emit_hold_or_empty_sample 对齐的失败输出策略，避免重复发送上一帧
    // H264+RTP 新时间戳（macOS VideoToolbox 等对参考链更敏感）。
    void apply_emit_fail_policy(rtc::binary& out_sample, bool request_idr);

    void reset_for_session_start();
    void try_recover_main_window(uint64_t now_unix_ms);
    void fill_capture_backend_telemetry(remote_capture_telemetry& out_telemetry) const;

    std::string m_exe_path;
    std::function<void()> m_on_remote_process_exit;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_exit_notified{false};

    PROCESS_INFORMATION m_pi{};
    DWORD m_capture_pid = 0;
    DWORD m_launch_pid = 0;
    HWND m_main_window = nullptr;

    std::thread m_exit_watch_thread;

    // 进程与窗口选择
    std::unique_ptr<RemoteProcessSession> m_process_session;
    std::string m_target_exe_base_name;
    bool m_allow_pid_rebind_by_exename = true;
    uint64_t m_pid_rebind_deadline_unix_ms = 0;

    // 采集与健康管理
    bool m_capture_all_windows = false;
    bool m_lock_capture_backend = true;
    uint32_t m_capture_degrade_ms = 2500;

    std::unique_ptr<GdiCapture> m_gdi_capture;
    std::unique_ptr<DXGICapture> m_dxgi_capture;
    std::unique_ptr<CaptureBackendState> m_capture_backend_state;

    // 编码
    int m_video_fps = 30;
    int m_encoder_layout_change_threshold_px = 8;
    int m_encoder_layout_change_required_streak = 5;

    std::unique_ptr<VideoEncodePipeline> m_video_encode_pipeline;
    BmpDumpWriter m_bmp_dump;
    bool m_had_successful_video = false;

    // 与 client 一致：steadyFrameHold = lock_backend && !locked_dxgi，仅在锁 GDI 时失败 tick 才重复上一帧。
    bool m_steady_frame_hold = false;

    // 失败时的 hold 或回退
    rtc::binary m_last_good_video_sample;
    bool m_have_last_good_sample = false;

    std::vector<uint8_t> m_last_good_rgb_frame;
    int m_last_good_rgb_w = 0;
    int m_last_good_rgb_h = 0;

    // 远端退出判定（窗口缺失）
    uint64_t m_window_missing_since_unix_ms = 0;
    uint32_t m_window_missing_exit_grace_ms = 0;
};

