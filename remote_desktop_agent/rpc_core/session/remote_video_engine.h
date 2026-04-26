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
#include "capture/i_capture_source.h"
#include "capture/process_ui_capture.h"
#include "common/process_ops.h"
#include "common/remote_video_contract.h"
#include "session/remote_video_engine_impl_types.h"

class VideoEncodePipeline;

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
    void notify_remote_exit_if_needed(const char* why);
    void notify_window_missing_if_needed(const char* why, uint64_t now_unix_ms);
    bool is_remote_process_still_running() const;

    void reset_for_session_start();

    void exit_watch_loop();
    void capture_loop();
    void encode_loop();

private:
    std::function<void()> m_on_remote_process_exit;
    window_missing_fn m_on_window_missing;

    
    std::atomic<bool> m_exit_notified{false};

    HWND m_main_window = nullptr;

    std::atomic<bool> m_running{false};
    std::thread m_exit_watch_thread; 
    std::atomic<bool> m_threads_running{false};
    std::thread m_capture_thread;
    std::thread m_encode_thread; 

    rpc_video_engine_impl::LatestRawFrame<CapturedRawFrameWithTelemetry> m_latest_frame;
    rpc_video_engine_impl::BoundedQueue<EncodedFrameWithTelemetry> m_latest_encoded;

	std::unique_ptr<process_ops> m_process_ops;

    std::unique_ptr<ICaptureSource> m_capture_source;

    int m_video_fps = 30;

    std::unique_ptr<VideoEncodePipeline> m_video_encode_pipeline;
    BmpDumpWriter m_bmp_dump;

	uint64_t m_window_missing_since_unix_ms = 0;//第一次检测到窗口缺失的时间戳，用于判断是否超过 grace 时间阈值以触发远程退出通知。
};
