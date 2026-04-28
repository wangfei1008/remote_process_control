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
#include "capture/session/process_session.h"
#include "common/remote_video_contract.h"
#include "session/remote_video_engine_impl_types.h"

class CaptureWorker;
class EncodeWorker;


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

public:
    void notify_remote_exit_if_needed(const char* why);
    void notify_window_missing_if_needed(const char* why, uint64_t now_unix_ms);

public:
    std::function<void()> m_on_remote_process_exit;
    window_missing_fn m_on_window_missing;
    
    std::atomic<bool> m_exit_notified{false};

    // capture 线程写；其他线程读
    std::atomic<HWND> m_main_window{nullptr};

    std::unique_ptr<CaptureWorker> m_capture_worker;
    std::unique_ptr<EncodeWorker> m_encode_worker;

    rpc_video_engine_impl::LatestRawFrame<CapturedRawFrameWithTelemetry> m_latest_frame;
    rpc_video_engine_impl::BoundedQueue<EncodedFrameWithTelemetry> m_latest_encoded;

    capture::ProcessSession m_session;

    int m_video_fps = 30;
};
