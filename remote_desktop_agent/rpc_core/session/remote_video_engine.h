#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <windows.h>

#include "rtc/rtc.hpp"

#include "capture/bmp_dump_writer.h"
#include "capture/backend/i_capture_backend.h"
#include "capture/pipeline/capture_pipeline.h"
#include "capture/pipeline/frame_composer.h"
#include "capture/pipeline/frame_filter.h"
#include "capture/policy/capture_target_resolver.h"
#include "capture/policy/window_score_policy.h"
#include "capture/session/process_session.h"
#include "capture/infra/win32_process.h"
#include "capture/infra/win32_window.h"
#include "common/remote_video_contract.h"
#include "session/remote_video_engine_impl_types.h"

class VideoEncodePipeline;

namespace remote_video_engine_detail {
class CaptureWorker;
class EncodeWorker;
} // namespace remote_video_engine_detail

struct CapturedRawFrameWithTelemetry {
    rpc_video_contract::RawFrame frame;
    rpc_video_contract::TelemetrySnapshot telem;
};

struct EncodedFrameWithTelemetry {
    rpc_video_contract::TelemetrySnapshot telem{};
    rtc::binary payload_storage{};
};

// 视频引擎：进程全界面采集（不完整帧丢弃）+ 编码
class remote_video_engine {
public:
    using window_missing_fn = std::function<void(const char* why, uint64_t missing_ms)>;
    remote_video_engine(const std::string& exe_path,  std::function<void()> on_remote_process_exit, window_missing_fn on_window_missing);
    ~remote_video_engine();

    void start();
    void stop();

    void produce_next_video_sample(rtc::binary& out_sample, rpc_video_contract::TelemetrySnapshot& out_telem);

    void request_force_keyframe();
    HWND get_main_window() const;

private:
    friend class remote_video_engine_detail::CaptureWorker;
    friend class remote_video_engine_detail::EncodeWorker;

    void notify_remote_exit_if_needed(const char* why);
    void notify_window_missing_if_needed(const char* why, uint64_t now_unix_ms);
    bool is_remote_process_still_running_from_snapshot() const;

    void ensure_capture_stack();
    void exit_watch_loop();
    void capture_loop();
    void encode_loop();

private:
    std::function<void()> m_on_remote_process_exit;
    window_missing_fn m_on_window_missing;
    
    std::atomic<bool> m_exit_notified{false};

    // capture 线程写；其他线程读
    std::atomic<HWND> m_main_window{nullptr};

    // pid 快照：capture 线程更新；exit_watch 线程只读（避免跨线程触碰 ProcessSession）
    std::atomic<DWORD> m_launch_pid_for_watch{0};
    std::atomic<DWORD> m_capture_pid_for_watch{0};
    std::atomic<bool> m_launch_running_for_watch{false};

    std::atomic<bool> m_running{false};
    std::thread m_exit_watch_thread;
    std::atomic<bool> m_threads_running{false};
    std::thread m_capture_thread;
    std::thread m_encode_thread; 

    rpc_video_engine_impl::LatestRawFrame<CapturedRawFrameWithTelemetry> m_latest_frame;
    rpc_video_engine_impl::BoundedQueue<EncodedFrameWithTelemetry> m_latest_encoded;

    capture::ProcessSession m_session;

    win32::Window m_wops;
    win32::Process m_prims;
    capture::WindowScorePolicy m_score_policy;
    capture::CaptureTargetResolver m_resolver;

    std::unique_ptr<capture::ICaptureBackend> m_backend;
    capture::FrameComposer m_composer;
    capture::FrameFilter m_filter;
    std::unique_ptr<capture::CapturePipeline> m_pipeline;

    int m_video_fps = 30;

    std::unique_ptr<VideoEncodePipeline> m_video_encode_pipeline;
};
