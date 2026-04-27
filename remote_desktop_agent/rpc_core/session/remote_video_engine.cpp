#include "session/remote_video_engine.h"

#include "app/runtime_config.h"
#include "capture/backend/capture_backend_factory.h"
#include "common/rpc_time.h"
#include "encode/video_encode_pipeline.h"
#include "input/input_controller.h"

#include <chrono>
#include <iostream>
#include <algorithm>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

namespace {

static void release_raw_frame_owned(rpc_video_contract::RawFrame& f)
{
    if (f.owned.release && f.owned.opaque) {
        f.owned.release(f.owned.opaque);
    }
    f.owned = {};
    f.plane_count = 0;
    for (auto& p : f.planes) p = {};
    if (f.gpu.release && f.gpu.opaque) {
        f.gpu.release(f.gpu.opaque);
    }
    f.gpu = {};
    f.ext = nullptr;
}

static BmpDumpDiag make_bmp_dump_diag_from_hw(bool used_hw)
{
    BmpDumpDiag d;
    d.use_hw_capture = used_hw;
    d.force_software_active = false;
    d.top_black_strip_streak = 0;
    d.dxgi_instability_score = 0;
    d.dxgi_disabled_for_session = false;
    return d;
}

} // namespace

remote_video_engine::remote_video_engine(const std::string& exe_path, std::function<void()> on_remote_process_exit, remote_video_engine::window_missing_fn on_window_missing)
    : m_on_remote_process_exit(std::move(on_remote_process_exit))
    , m_on_window_missing(std::move(on_window_missing))
    , m_session(capture::ProcessLaunchConfig{ exe_path, 0, true })
    , m_resolver(capture::CaptureTargetResolver::Deps{ m_wops, m_prims, m_score_policy })
{
    m_video_encode_pipeline = std::make_unique<VideoEncodePipeline>();
    m_bmp_dump.configure_from_config();

    m_video_fps = (std::max)(1, runtime_config::get_int("RPC_ACTIVE_FPS", 30));
    m_video_encode_pipeline->configure(m_video_fps);

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

bool remote_video_engine::is_remote_process_still_running() const
{
    if (m_session.is_launch_running()) return true;
    if (m_prims.is_running(m_session.capture_pid())) return true;
    if (m_prims.is_running(m_session.launch_pid())) return true;
    return false;
}

void remote_video_engine::notify_window_missing_if_needed(const char* why, uint64_t now_unix_ms)
{
    if (!m_on_window_missing) return;
    const uint64_t since = m_window_missing_since_unix_ms ? m_window_missing_since_unix_ms : now_unix_ms;
    const uint64_t missing_ms = (now_unix_ms >= since) ? (now_unix_ms - since) : 0;
    try {
        m_on_window_missing(why ? why : "", missing_ms);
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

    reset_for_session_start();

    m_main_window = nullptr;
    if (!m_session.start()) {
        m_running = false;
        return;
    }

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
    try {
        std::cout << "[proc] remote exit notify why=" << (why ? why : "")
			      << " capture_pid=" << m_session.capture_pid()  
                  << " main_hwnd=" << static_cast<void*>(m_main_window)
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
    while (m_running.load(std::memory_order_relaxed)) {
        const auto target = m_resolver.resolve({
            m_session.capture_pid(),
            m_session.launch_pid(),
            m_session.target_basename_lower(),
            m_main_window,
            true
        });
        if (target.diag.pid_rebound)
            m_session.rebind_capture_pid(target.capture_pid);
        m_main_window = target.main_hwnd ? target.main_hwnd : m_main_window;

        const DWORD owner_pid = target.main_hwnd_owner_pid;
        const DWORD cap_pid = target.capture_pid;

        if (owner_pid != 0) {
            auto h = m_prims.open(owner_pid, PROCESS_QUERY_LIMITED_INFORMATION);
            if (!h) {
                notify_remote_exit_if_needed("main_window_owner_open_failed");
                break;
            }
            if (!m_prims.is_running(h.get())) {
                notify_remote_exit_if_needed("main_window_owner_exited");
                break;
            }
        } else if (cap_pid != 0) {
            if (!m_prims.is_running(cap_pid)) {
                notify_remote_exit_if_needed("capture_pid_exited");
                break;
            }
        } else {
            if (!m_session.is_launch_running()) {
                const DWORD cap = m_session.capture_pid();
                const bool capturing_child = (cap != 0 && cap != m_session.launch_pid() && m_prims.is_running(cap));
                if (!capturing_child) {
                    notify_remote_exit_if_needed("launch_process_exited");
                    break;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}


//采集生产者,不断抓取进程 UI 画面（DXGI/GDI）并合成 RGB 帧，写入最新帧槽位。
void remote_video_engine::capture_loop()
{
    // pacing
    const int fps = (std::max)(1, m_video_fps);
    const auto frame_period = std::chrono::microseconds(1000000 / fps);
    auto next_tick = std::chrono::steady_clock::now();
    uint64_t frame_id_seq = 1;
    const bool filter_black = runtime_config::get_bool("RPC_FILTER_CAPTURE_BLACK_FRAMES", true);
    while (m_threads_running.load(std::memory_order_acquire)) {
        const auto now_unix_ms = rpc_unix_epoch_ms();

        if (!m_pipeline || !m_backend) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        //1. Resolve capture PID + surfaces (handles PID drift / rebind).
        const auto target = m_resolver.resolve({ m_session.capture_pid(),  m_session.launch_pid(),  m_session.target_basename_lower(), m_main_window,true});
        if (target.diag.pid_rebound)   m_session.rebind_capture_pid(target.capture_pid);

        if (target.surfaces.empty()) {
            m_main_window = nullptr;
            if (m_window_missing_since_unix_ms == 0) m_window_missing_since_unix_ms = now_unix_ms;
            if (now_unix_ms - m_window_missing_since_unix_ms >= 5000) {
				m_window_missing_since_unix_ms = 0;// reset timer after grace expired
                if (is_remote_process_still_running()) {
                    notify_window_missing_if_needed("no_surfaces_grace_expired_but_process_alive", now_unix_ms);
                } else {
                    notify_remote_exit_if_needed("no_surfaces_grace_expired");
                    m_running = false;
                    m_threads_running.store(false, std::memory_order_release);
                }
            }
            std::cout << "[capture] no surfaces found for pid=" << m_session.capture_pid()
                      << " main_window=" << static_cast<void*>(m_main_window)
                      << " resolver_why=" << (target.diag.reason ? target.diag.reason : "")
                      << " prev_pid=" << target.diag.previous_capture_pid
                      << " owner_pid=" << target.main_hwnd_owner_pid
                      << " pid_rebound=" << (target.diag.pid_rebound ? 1 : 0)
                      << " main_from_surfaces=" << (target.diag.selected_from_surfaces ? 1 : 0)
				<< std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        m_main_window = target.main_hwnd;

        //2. Capture and compose.
        const uint64_t prep_unix_ms = rpc_unix_epoch_ms();
        const uint64_t frame_id = frame_id_seq;
        rpc_video_contract::RawFrame rf;
        rpc_video_contract::TelemetrySnapshot telem;
        
        if (m_pipeline->grab_raw_frame(target.surfaces, now_unix_ms, prep_unix_ms, frame_id, filter_black, rf, telem)) {
            auto* vec = static_cast<std::vector<uint8_t>*>(rf.owned.opaque);
            const int w = rf.coded_size.w;
            const int h = rf.coded_size.h;
			frame_id_seq++;
            
            // Sanitize in capture thread; do not treat drops as session health failures.
            input_controller::instance()->set_capture_screen_rect(rf.visible_rect.x, rf.visible_rect.y, w, h);

            const BmpDumpDiag bmp_diag = make_bmp_dump_diag_from_hw(telem.backend == rpc_video_contract::CaptureBackend::Dxgi);
            m_bmp_dump.dump_capture_if_needed(*vec, w, h, bmp_diag);

            CapturedRawFrameWithTelemetry pkt;
            pkt.frame = std::move(rf);
            pkt.telem = telem;

            {
                std::lock_guard<std::mutex> lk(m_latest_frame.mtx);
                if (m_latest_frame.latest.has_value()) {
                    release_raw_frame_owned(m_latest_frame.latest->frame);
                }
                m_latest_frame.latest = std::move(pkt);
            }
            m_latest_frame.cv.notify_one();
        }else
            std::cout << "[capture] grab_raw_frame failed for frame_id=" << frame_id_seq	<< std::endl;

        // Pacing.
        next_tick += frame_period;
        const auto now = std::chrono::steady_clock::now();
        if (next_tick > now) {
            std::this_thread::sleep_for(next_tick - now);
        } else {
            // If we're behind, skip ahead to avoid drift.
            next_tick = now;
        }
    }
}

//编码生产者/样本生产者, 等待最新帧槽位有新帧（或超时），取出后编码并推入最新编码队列。
void remote_video_engine::encode_loop()
{
    uint64_t last_encoded_id = 0;

    while (m_threads_running.load(std::memory_order_acquire)) {
        std::optional<CapturedRawFrameWithTelemetry> packet;
        {
            //1.等待新帧（最多 50ms）
            std::unique_lock<std::mutex> lk(m_latest_frame.mtx);
            m_latest_frame.cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                return !m_threads_running.load(std::memory_order_acquire) ||
                       (m_latest_frame.latest.has_value() && m_latest_frame.latest->frame.frame_id != last_encoded_id);
            });
            if (!m_threads_running.load(std::memory_order_acquire)) break;
            if (!m_latest_frame.latest.has_value()) continue;
            if (m_latest_frame.latest->frame.frame_id == last_encoded_id) continue;
            
            //2.“拿走最新帧”并立即释放锁Take a snapshot of the latest (copy/move rgb to avoid holding lock during encode).
            packet = std::move(m_latest_frame.latest.value());
            // Keep the slot populated with an empty placeholder so overwrite count remains meaningful.
            m_latest_frame.latest.reset();
        }

        //3.确认编码管线存在
        if (!packet.has_value() || !m_video_encode_pipeline) {
            if (packet.has_value()) release_raw_frame_owned(packet->frame);
            continue;
        }

        { // backlog guard
            std::lock_guard<std::mutex> lk(m_latest_encoded.mtx);
            if (m_latest_encoded.q.size() > 3) {
                std::cout << "[encode] dropping frame_id=" << packet->frame.frame_id << " backlog=" << m_latest_encoded.q.size() << std::endl;
                release_raw_frame_owned(packet->frame);
                continue;
            }
        }
        CapturedRawFrameWithTelemetry& pkt = packet.value();
        rpc_video_contract::RawFrame& rf = pkt.frame;
        last_encoded_id = rf.frame_id;

        const int captured_w = rf.coded_size.w;
        const int captured_h = rf.coded_size.h;
        int target_w = captured_w;
        int target_h = captured_h;
        bool applied_layout = false;
        //4.确保编码器布局/分辨率策略一致
        const bool layout_ok = m_video_encode_pipeline->ensure_encoder_layout(captured_w, captured_h, target_w, target_h, applied_layout);
        if (!layout_ok) {
            std::cout << "[encode] unsupported frame layout: frame_id=" << rf.frame_id << std::endl;
            release_raw_frame_owned(rf);
            continue;
        }

        //5.编码
        auto* vec = static_cast<std::vector<uint8_t>*>(rf.owned.opaque);
        if (!vec) {
            std::cout << "[encode] frame missing owned rgb buffer: frame_id=" << rf.frame_id << std::endl;
            release_raw_frame_owned(rf);
			
            continue;
        }
        VideoEncodeResult enc = m_video_encode_pipeline->encode_frame(*vec, captured_w, captured_h, applied_layout);

        if (enc.encode_ok && !enc.sample.empty() && !enc.invalid_payload) {
            EncodedFrameWithTelemetry out;
            out.payload_storage = std::move(enc.sample);

            out.telem = pkt.telem;
            out.telem.capture_size = rpc_video_contract::VideoSize{ target_w, target_h };
            out.telem.encode_unix_ms = enc.frame_unix_ms ? enc.frame_unix_ms : rpc_unix_epoch_ms();

            {
                //5.1 推入最新编码队列
                std::lock_guard<std::mutex> lk(m_latest_encoded.mtx);
                m_latest_encoded.q.push_back(std::move(out));
            }
        } else {
            // If encode fails but we already have a good sample, request keyframe to recover quickly.
            std::cout << "[encode] encode failed for frame_id=" << rf.frame_id << std::endl;
            request_force_keyframe();
        }

        // RawFrame owns heap buffer; always release after encoding attempt.
        release_raw_frame_owned(rf);
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
    if (!m_video_encode_pipeline) return;
    try {
        m_video_encode_pipeline->request_force_keyframe_with_cooldown(rpc_unix_epoch_ms());
    } catch (...) {
    }
}

HWND remote_video_engine::get_main_window() const
{
    return m_main_window;
}

void remote_video_engine::reset_for_session_start()
{
    m_window_missing_since_unix_ms = 0;

    if (m_video_encode_pipeline) m_video_encode_pipeline->reset_for_stream_start();
    m_bmp_dump.reset_session();
}
