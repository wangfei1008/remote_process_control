#include "session/remote_desktop_media_session.h"

#include "session/remote_video_engine.h"
#include "source/silence_opus_generator.h"
#include "transport/client_peer_connection.h"
#include "transport/remote_desktop_media_sender.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <utility>

struct remote_desktop_media_session_impl {
    std::shared_ptr<remote_video_engine> m_video_engine;
    std::shared_ptr<silence_opus_generator> m_audio_generator;
    std::shared_ptr<remote_desktop_media_sender> m_sender;

    std::thread m_loop_thread;
    std::atomic<bool> m_loop_running{false};

    uint64_t m_start_time_us = 0;

    // skeleton：先固定节奏，后续把真实 fps/编码 pacing 接回去
    uint64_t m_video_frame_duration_us = 33333; // ~30fps
    uint64_t m_audio_frame_duration_us = 20000; // 20ms opus
    uint64_t m_next_video_time_us = 0;
    uint64_t m_next_audio_time_us = 0;
};

static uint64_t current_time_in_microseconds()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

remote_desktop_media_session::remote_desktop_media_session(std::string exe_path,
                                                           runtime_settings stream_settings,
                                                           std::function<void()> on_remote_process_exit,
                                                           std::function<void(const char* why, uint64_t missing_ms)> on_window_missing,
                                                           std::function<void()> stop_if_no_clients)
    : m_exe_path(std::move(exe_path))
    , m_stream_settings(std::move(stream_settings))
    , m_on_remote_process_exit(std::move(on_remote_process_exit))
    , m_on_window_missing(std::move(on_window_missing))
    , m_stop_if_no_clients(std::move(stop_if_no_clients))
{
    m_impl = std::make_unique<remote_desktop_media_session_impl>();

    m_impl->m_video_engine = std::make_shared<remote_video_engine>(m_exe_path, m_on_remote_process_exit, m_on_window_missing);
    m_impl->m_audio_generator = std::make_shared<silence_opus_generator>(m_impl->m_audio_frame_duration_us);

    auto snapshot_clients = [this]() -> std::vector<std::shared_ptr<ClientPeerConnection>> {
        std::lock_guard<std::mutex> lk(m_mutex);
        std::vector<std::shared_ptr<ClientPeerConnection>> out;
        const auto peer = m_client_peer.lock();
        if (!peer) return out;
        out.push_back(peer);
        return out;
    };

    m_impl->m_sender = std::make_shared<remote_desktop_media_sender>(
        m_stream_settings.frame_mark_interval,
        m_stream_settings.capture_health_interval,
        std::move(snapshot_clients),
        m_stop_if_no_clients);
}

remote_desktop_media_session::~remote_desktop_media_session()
{
    stop_media_session();
}

void remote_desktop_media_session::attach_client_peer(const std::weak_ptr<ClientPeerConnection>& client_peer)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_client_peer = client_peer;
}

void remote_desktop_media_session::start_media_session()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_impl) return;
    if (m_impl->m_loop_running.load(std::memory_order_relaxed)) return;

    m_impl->m_video_engine->start();
    m_impl->m_audio_generator->start();

    m_impl->m_start_time_us = current_time_in_microseconds();
    // 首帧应立刻调度：若写成 next_video=duration，首轮会先睡满一帧间隔（且常让 20ms 音频排在 33ms 视频前），出画明显变慢。
    // 旧版 Stream 在首次 get_sample 时即抓编码，无此相位延迟。
    m_impl->m_next_video_time_us = 0;
    m_impl->m_next_audio_time_us = 0;

    m_impl->m_loop_running = true;

    m_impl->m_loop_thread = std::thread([this]() {
        while (m_impl && m_impl->m_loop_running.load(std::memory_order_relaxed)) {
            const uint64_t next_time = (m_impl->m_next_video_time_us <= m_impl->m_next_audio_time_us)
                ? m_impl->m_next_video_time_us
                : m_impl->m_next_audio_time_us;

            const uint64_t elapsed = current_time_in_microseconds() - m_impl->m_start_time_us;
            if (next_time > elapsed) {
                std::this_thread::sleep_for(std::chrono::microseconds(next_time - elapsed));
            }

            if (!m_impl || !m_impl->m_loop_running.load(std::memory_order_relaxed)) break;

            if (m_impl->m_next_video_time_us <= m_impl->m_next_audio_time_us) {
                rtc::binary video_sample;
                remote_capture_telemetry telemetry;

                static std::uint64_t s_video_loop_tick = 0;
                ++s_video_loop_tick;

                const auto t_produce0 = std::chrono::steady_clock::now();
                m_impl->m_video_engine->produce_next_video_sample(video_sample, telemetry);
                const auto t_produce1 = std::chrono::steady_clock::now();
                m_impl->m_sender->on_video_sample(m_impl->m_next_video_time_us, video_sample, telemetry);
                const auto t_after_send = std::chrono::steady_clock::now();

                const auto produce_us = static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t_produce1 - t_produce0).count());
                const auto send_us = static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t_after_send - t_produce1).count());

                static std::chrono::steady_clock::time_point s_last_nonempty_log{};
                if (!video_sample.empty()) {
                    if (t_after_send - s_last_nonempty_log >= std::chrono::seconds(1) || s_video_loop_tick <= 5) {
                        s_last_nonempty_log = t_after_send;
                        std::cout << "[latency][agent_pipe] tick=" << s_video_loop_tick
                                  << " produce_us=" << produce_us << " send_path_us=" << send_us
                                  << " cap_ms=" << telemetry.last_capture_ms
                                  << " enc_ms=" << telemetry.last_encode_ms
                                  << " bytes=" << video_sample.size() << std::endl;
                    }
                } else if (s_video_loop_tick <= 12) {
                    std::cout << "[latency][agent_pipe] tick=" << s_video_loop_tick
                              << " produce_us=" << produce_us << " send_path_us=" << send_us << " (empty)\n";
                }

                m_impl->m_next_video_time_us += m_impl->m_video_frame_duration_us;
            } else {
                rtc::binary audio_sample;
                m_impl->m_audio_generator->produce_next_audio_sample(audio_sample);
                m_impl->m_sender->on_audio_sample(m_impl->m_next_audio_time_us, audio_sample);
                m_impl->m_next_audio_time_us += m_impl->m_audio_frame_duration_us;
            }
        }
    });
}

void remote_desktop_media_session::stop_media_session()
{
    std::thread loop_to_join;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_impl) return;
        if (!m_impl->m_loop_running.exchange(false, std::memory_order_relaxed)) return;
        if (m_impl->m_loop_thread.joinable()) loop_to_join = std::move(m_impl->m_loop_thread);
    }

    if (loop_to_join.joinable()) loop_to_join.join();

    try {
        if (m_impl && m_impl->m_audio_generator) m_impl->m_audio_generator->stop();
        if (m_impl && m_impl->m_video_engine) m_impl->m_video_engine->stop();
    } catch (...) {}
}

void remote_desktop_media_session::request_force_keyframe()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_impl || !m_impl->m_video_engine) return;
    try {
        m_impl->m_video_engine->request_force_keyframe();
    } catch (...) {}
}

bool remote_desktop_media_session::is_media_running() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_impl) return false;
    return m_impl->m_loop_running.load(std::memory_order_relaxed);
}

HWND remote_desktop_media_session::get_main_window() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_impl || !m_impl->m_video_engine) return nullptr;
    return m_impl->m_video_engine->get_main_window();
}


