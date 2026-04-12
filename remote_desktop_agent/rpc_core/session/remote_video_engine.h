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
#include "capture/capture_kind_resolver.h"
#include "capture/i_capture_source.h"
#include "capture/process_ui_capture.h"
#include "session/remote_capture_telemetry.h"

class RemoteProcessSession;

class VideoEncodePipeline;

// 视频引擎：进程全界面采集（不完整帧丢弃）+ 编码
class remote_video_engine {
public:
    using window_missing_fn = std::function<void(const char* why, uint64_t missing_ms)>;
    remote_video_engine(std::string exe_path,
                        std::function<void()> on_remote_process_exit,
                        window_missing_fn on_window_missing);
    ~remote_video_engine();

    void start();
    void stop();

    void produce_next_video_sample(rtc::binary& out_sample, remote_capture_telemetry& out_telemetry);

    void request_force_keyframe();
    HWND get_main_window() const;

private:
    void notify_remote_exit_if_needed(const char* why);
    void notify_window_missing_if_needed(const char* why, uint64_t now_unix_ms);
    bool is_remote_process_still_running() const;
    void exit_watch_loop();

    void apply_emit_fail_policy(rtc::binary& out_sample, bool request_idr);

    void reset_for_session_start();
    void try_recover_main_window(uint64_t now_unix_ms);
    void fill_capture_backend_telemetry(remote_capture_telemetry& out_telemetry) const;
    /// 对远程主窗口执行一次最大化 + 置顶（每会话每个 HWND 仅做一次，可经 RPC_LAUNCH_WINDOW_* 关闭）。
    void apply_launch_window_placement(HWND hwnd);

    std::string m_exe_path;
    std::function<void()> m_on_remote_process_exit;
    window_missing_fn m_on_window_missing;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_exit_notified{false};

    PROCESS_INFORMATION m_pi{};
    DWORD m_capture_pid = 0;
    DWORD m_launch_pid = 0;
    HWND m_main_window = nullptr;
    HWND m_last_launch_placement_hwnd = nullptr;

    std::thread m_exit_watch_thread;

    std::unique_ptr<RemoteProcessSession> m_process_session;
    std::string m_target_exe_base_name;
    bool m_allow_pid_rebind_by_exename = true;
    uint64_t m_pid_rebind_deadline_unix_ms = 0;

    /// 引擎构造时解析，整引擎生命周期内不变；AUTO 为 DXGI > GDI。
    /// 显式 dxgi 若 probe 失败：不创建采集后端、不回退 GDI，start() 将拒绝启动（见 m_capture_explicit_backend_error）。
    ProcessCaptureKind m_capture_kind = ProcessCaptureKind::Gdi;
    bool m_capture_explicit_backend_error = false;
    ProcessUiCaptureOptions m_ui_capture_options;

    /// 与 m_capture_kind 对应的唯一采集实现（阶段 B：ICaptureSource）。
    std::unique_ptr<ICaptureSource> m_capture_source;

    int m_video_fps = 30;
    int m_encoder_layout_change_threshold_px = 8;
    int m_encoder_layout_change_required_streak = 5;

    std::unique_ptr<VideoEncodePipeline> m_video_encode_pipeline;
    BmpDumpWriter m_bmp_dump;
    bool m_had_successful_video = false;

    bool m_steady_frame_hold = false;

    rtc::binary m_last_good_video_sample;
    bool m_have_last_good_sample = false;

    std::vector<uint8_t> m_last_good_rgb_frame;
    int m_last_good_rgb_w = 0;
    int m_last_good_rgb_h = 0;

    uint64_t m_window_missing_since_unix_ms = 0;
    uint32_t m_window_missing_exit_grace_ms = 0;
    uint64_t m_last_window_missing_notify_unix_ms = 0;

    /// 上一帧 grab 是否走 HW（DXGI），供遥测。
    bool m_last_capture_used_hw = false;
};
