#include "session/remote_video_engine.h"
#include "session/capture_worker.h"
#include "session/encode_worker.h"

#include "app/runtime_config.h"
#include "common/rpc_time.h"
#include "input/input_controller.h"

#include <chrono>
#include <iostream>
#include <algorithm>
#include <thread>

remote_video_engine::remote_video_engine(const std::string& exe_path, std::function<void()> on_remote_process_exit, remote_video_engine::window_missing_fn on_window_missing)
    : m_on_remote_process_exit(std::move(on_remote_process_exit))
    , m_on_window_missing(std::move(on_window_missing))
    , m_session(capture::ProcessLaunchConfig{ exe_path, 0, true })
{
    m_video_fps = (std::max)(1, runtime_config::get_int("RPC_ACTIVE_FPS", 30));
    m_latest_encoded.set_max_size(3);
}

remote_video_engine::~remote_video_engine()
{
    stop();
}

void remote_video_engine::notify_window_missing_if_needed(const char* why, uint64_t now_unix_ms)
{
    if (!m_on_window_missing) return;
    try {
        m_on_window_missing(why ? why : "", 5000);
    } catch (...) {
    }
}

void remote_video_engine::start()
{    
    m_main_window.store(nullptr, std::memory_order_release);
    if (!m_session.start())  return;

    m_exit_notified = false;

    // Capture/encode pipeline threads
    m_capture_worker = std::make_unique<CaptureWorker>(*this);
    m_capture_worker->start();
    m_encode_worker = std::make_unique<EncodeWorker>(*this);
    m_encode_worker->start();
}

void remote_video_engine::stop()
{
    // Wake encode thread if waiting.
    {
        std::lock_guard<std::mutex> lk(m_latest_frame.mtx);
        m_latest_frame.cv.notify_all();
    }

    if (m_capture_worker) {
        m_capture_worker->stop();
        m_capture_worker->join();
        m_capture_worker.reset();
    }
    if (m_encode_worker) {
        m_encode_worker->stop();
        m_encode_worker->join();
        m_encode_worker.reset();
    }

    input_controller::instance()->set_capture_screen_rect(0, 0, 0, 0);

    try {
        std::cout << "[proc] stop: terminating processes launch_pid=" << m_session.launch_pid()
                  << " capture_pid=" << m_session.capture_pid() << std::endl;
        m_session.stop();
    } catch (...) {
    }
}

void remote_video_engine::notify_remote_exit_if_needed(const char* why)
{
    if (m_exit_notified.exchange(true)) return;
    const HWND hwnd_snapshot = m_main_window.load(std::memory_order_acquire);
    try {
        std::cout << "[proc] remote exit notify why=" << (why ? why : "")
                  << " main_hwnd=" << static_cast<void*>(hwnd_snapshot)
                  << std::endl;
    } catch (...) {
    }
    if (m_on_remote_process_exit) {
        try {
            m_on_remote_process_exit();
        } catch (...) {
        }
    }
}


//MediaSession 调度线程，样本消费者,每个视频 tick 来“取一次样本”，把样本交给 sender（外部调用它的人会把 out_sample 传给 m_sender->on_video_sample(...)）。
void remote_video_engine::produce_next_video_sample(rtc::binary& out_sample, rpc_video_contract::TelemetrySnapshot& out_telem)
{
    out_sample.clear();
    out_telem = rpc_video_contract::TelemetrySnapshot{};

    //1.无阻塞地“取走最新编码样本”Pop latest encoded sample (no blocking). If none, apply steady-hold fallback policy.
    EncodedFrameWithTelemetry es;
    bool have = false;
    {
        std::lock_guard<std::mutex> lk(m_latest_encoded.mtx);
        if (!m_latest_encoded.q.empty()) {
            es = std::move(m_latest_encoded.q.front());
			m_latest_encoded.q.pop_front();
            have = true;
        }
    }

    //2.若拿到了有效样本：填充输出并返回
    if (have && !es.payload_storage.empty()) {
        out_sample = std::move(es.payload_storage);
        out_telem = es.telem;
        return;
    }
}

void remote_video_engine::request_force_keyframe()
{
    if (!m_encode_worker) return;
    m_encode_worker->request_force_keyframe();
}

HWND remote_video_engine::get_main_window() const
{
    return m_main_window.load(std::memory_order_acquire);
}
