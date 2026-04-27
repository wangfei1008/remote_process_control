#include "session/remote_video_engine.h"
#include "session/remote_video_engine_workers.h"

#include "app/runtime_config.h"
#include "capture/backend/capture_backend_factory.h"
#include "common/rpc_time.h"
#include "encode/video_encode_pipeline.h"
#include "input/input_controller.h"

#include <chrono>
#include <iostream>
#include <algorithm>
#include <thread>

remote_video_engine::remote_video_engine(const std::string& exe_path, std::function<void()> on_remote_process_exit, remote_video_engine::window_missing_fn on_window_missing)
    : m_on_remote_process_exit(std::move(on_remote_process_exit))
    , m_on_window_missing(std::move(on_window_missing))
    , m_session(capture::ProcessLaunchConfig{ exe_path, 0, true })
    , m_resolver(capture::CaptureTargetResolver::Deps{ m_wops, m_prims, m_score_policy })
{
    m_video_encode_pipeline = std::make_unique<VideoEncodePipeline>();
    
    m_video_fps = (std::max)(1, runtime_config::get_int("RPC_ACTIVE_FPS", 30));
    m_video_encode_pipeline->configure(m_video_fps);
    m_latest_encoded.set_max_size(3);

    ensure_capture_stack();
    if (m_backend) {
        std::cout << "[capture] capture_kind=" << static_cast<int>(m_backend->kind()) << "\n";
    }
}

void remote_video_engine::ensure_capture_stack()
{
    if (m_backend && m_pipeline) return;
    m_pipeline.reset();
    m_backend = capture::create_capture_backend_from_config();
    if (!m_backend) return;
    m_pipeline = std::make_unique<capture::CapturePipeline>(capture::CapturePipeline::Deps{ *m_backend, m_composer, m_filter });
}

bool remote_video_engine::is_remote_process_still_running_from_snapshot() const
{
    if (m_launch_running_for_watch.load(std::memory_order_acquire)) return true;
    const DWORD launch_pid = m_launch_pid_for_watch.load(std::memory_order_acquire);
    const DWORD capture_pid = m_capture_pid_for_watch.load(std::memory_order_acquire);
    if (capture_pid != 0 && m_prims.is_running(capture_pid)) return true;
    if (launch_pid != 0 && m_prims.is_running(launch_pid)) return true;
    return false;
}


void remote_video_engine::notify_window_missing_if_needed(const char* why, uint64_t now_unix_ms)
{
    if (!m_on_window_missing) return;
    try {
        m_on_window_missing(why ? why : "", 5000);
    } catch (...) {
    }
}

remote_video_engine::~remote_video_engine()
{
    stop();
}

void remote_video_engine::start()
{    
    if (m_running.exchange(true)) return;

    if (m_video_encode_pipeline) m_video_encode_pipeline->reset_for_stream_start();

    m_main_window.store(nullptr, std::memory_order_release);
    if (!m_session.start()) {
        m_running = false;
        return;
    }
    // 由 capture 线程后续更新；exit_watch 线程只读这些快照，避免触碰 ProcessSession
    m_launch_pid_for_watch.store(m_session.launch_pid(), std::memory_order_release);
    m_capture_pid_for_watch.store(m_session.capture_pid(), std::memory_order_release);
    m_launch_running_for_watch.store(m_session.is_launch_running(), std::memory_order_release);

    ensure_capture_stack();
    if (!m_pipeline || !m_backend) {
        m_running = false;
        m_session.stop();
        return;
    }
    m_backend->on_new_session();

    m_exit_notified = false;
    m_exit_watch_thread = std::thread(&remote_video_engine::exit_watch_loop, this);

    // Capture/encode pipeline threads
    m_threads_running.store(true, std::memory_order_release);
    m_capture_thread = std::thread(&remote_video_engine::capture_loop, this);
    m_encode_thread = std::thread(&remote_video_engine::encode_loop, this);
}

void remote_video_engine::stop()
{
    m_running = false;
    m_threads_running.store(false, std::memory_order_release);
    m_launch_pid_for_watch.store(0, std::memory_order_release);
    m_capture_pid_for_watch.store(0, std::memory_order_release);
    m_launch_running_for_watch.store(false, std::memory_order_release);

    // Wake encode thread if waiting.
    {
        std::lock_guard<std::mutex> lk(m_latest_frame.mtx);
        m_latest_frame.cv.notify_all();
    }

    if (m_exit_watch_thread.joinable()) {
        m_exit_watch_thread.join();
    }
    if (m_capture_thread.joinable()) {
        m_capture_thread.join();
    }
    if (m_encode_thread.joinable()) {
        m_encode_thread.join();
    }

    m_pipeline.reset();
    m_backend.reset();

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
    const DWORD cap_pid_snapshot = m_capture_pid_for_watch.load(std::memory_order_acquire);
    try {
        std::cout << "[proc] remote exit notify why=" << (why ? why : "")
			      << " capture_pid=" << cap_pid_snapshot
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

void remote_video_engine::exit_watch_loop()
{
    return;
    // 只检查进程是否还活着：不 resolve，不访问 ProcessSession，不读写 HWND
    while (m_running.load(std::memory_order_relaxed)) {
        if (!is_remote_process_still_running_from_snapshot()) {
            notify_remote_exit_if_needed("watch_pids_not_running");
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

//采集生产者,不断抓取进程 UI 画面（DXGI/GDI）并合成 RGB 帧，写入最新帧槽位。
void remote_video_engine::capture_loop()
{
    remote_video_engine_detail::CaptureWorker(*this).run();
}

//编码生产者/样本生产者, 等待最新帧槽位有新帧（或超时），取出后编码并推入最新编码队列。
void remote_video_engine::encode_loop()
{
    remote_video_engine_detail::EncodeWorker(*this).run();
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
    if (!m_video_encode_pipeline) return;
    try {
        m_video_encode_pipeline->request_force_keyframe_with_cooldown(rpc_unix_epoch_ms());
    } catch (...) {
    }
}

HWND remote_video_engine::get_main_window() const
{
    return m_main_window.load(std::memory_order_acquire);
}
