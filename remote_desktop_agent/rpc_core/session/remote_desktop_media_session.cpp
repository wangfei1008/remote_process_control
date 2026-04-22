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

remote_desktop_media_session::remote_desktop_media_session(const std::string& exe_path,
                                                           runtime_settings stream_settings,
                                                           std::function<void()> on_remote_process_exit,
                                                           std::function<void(const char* why, uint64_t missing_ms)> on_window_missing,
                                                           std::function<void()> stop_if_no_clients)
    : m_stream_settings(std::move(stream_settings))
    , m_on_remote_process_exit(std::move(on_remote_process_exit))
    , m_on_window_missing(std::move(on_window_missing))
    , m_stop_if_no_clients(std::move(stop_if_no_clients))
{
    m_impl = std::make_unique<remote_desktop_media_session_impl>();

    m_impl->m_video_engine = std::make_shared<remote_video_engine>(exe_path, m_on_remote_process_exit, m_on_window_missing);
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

    // 线程模型概览：
    // - remote_video_engine 内部会再启动自己的采集/编码/退出监控线程（见 remote_video_engine::start）
    // - 本类再起一个 “发送调度线程” m_loop_thread：以单一时间轴把音频(20ms)与视频(~33ms)节奏化产出并交给 sender
    // - stop_media_session() 通过原子标志让循环尽快退出，并在锁外 join，保证析构安全且避免死锁
    m_impl->m_video_engine->start();
    m_impl->m_audio_generator->start();

    m_impl->m_start_time_us = current_time_in_microseconds();
    // 首帧应立刻调度：若写成 next_video=duration，首轮会先睡满一帧间隔（且常让 20ms 音频排在 33ms 视频前），出画明显变慢。
    // 旧版 Stream 在首次 get_sample 时即抓编码，无此相位延迟。
    m_impl->m_next_video_time_us = 0;
    m_impl->m_next_audio_time_us = 0;

    // 只用作“是否退出循环”的轻量信号，不承担跨线程发布复杂对象的职责，因此 memory_order_relaxed 足够。
    m_impl->m_loop_running = true;

    // 注意：线程 lambda 捕获 this。
    // - 由于析构会调用 stop_media_session() 并 join 该线程，因此 this 在 join 完成前必须保持存活。
    // - 循环里同时检查 m_impl 指针与 m_loop_running：前者防止实现体被释放后继续访问，后者用于正常停止路径。
    m_impl->m_loop_thread = std::thread([this]() {
        while (m_impl && m_impl->m_loop_running.load(std::memory_order_relaxed)) {
            // 选择下一次要“到点”的媒体类型（音频 or 视频），用同一个 start_time_us 做基准。
            // 这相当于一个非常简化的 pacing：谁的 next_time 更早就先产出谁。
            // 这里用“理想时间轴”(next_*) 而不是 “now+duration”：
            // - 可抵抗 sleep 抖动：即使某次晚了，next_* 仍按固定步长推进，不会越拖越慢
            // - 音视频在同一基准下比较，避免两个独立节拍器长期漂移
            const uint64_t next_time = (m_impl->m_next_video_time_us <= m_impl->m_next_audio_time_us)
                ? m_impl->m_next_video_time_us
                : m_impl->m_next_audio_time_us;

            // elapsed：距 start_time_us 的相对时间（微秒）。next_time 也是相对时间。
            const uint64_t elapsed = current_time_in_microseconds() - m_impl->m_start_time_us;
            if (next_time > elapsed) {
                // sleep_for 不是硬实时：系统调度/抢占可能造成抖动；这里的 next_* 会持续累加，
                // 让长时间漂移不会无限扩大（下一轮仍以“理想时间轴”推进）。
                std::this_thread::sleep_for(std::chrono::microseconds(next_time - elapsed));
            }

            // 二次检查：睡眠期间 stop 可能发生；这里尽快退出，避免在停止后仍产出/发送一帧。
            if (!m_impl || !m_impl->m_loop_running.load(std::memory_order_relaxed)) break;

            if (m_impl->m_next_video_time_us <= m_impl->m_next_audio_time_us) {
                rtc::binary video_sample;
                rpc_video_contract::TelemetrySnapshot telemetry;
                // produce：从 video_engine 拉取“下一帧编码后数据”（可能为空，例如窗口缺失/无有效帧）。
                m_impl->m_video_engine->produce_next_video_sample(video_sample, telemetry);

                // sender 负责把 sample 分发给当前 peer(s)（内部会自行处理无客户端/背压等策略）。
                m_impl->m_sender->on_video_sample(m_impl->m_next_video_time_us, video_sample, telemetry);
  
                // 推进理想视频时间轴：始终按固定帧间隔累加，不用“当前时刻”，从而避免抖动导致的节奏漂移。
                m_impl->m_next_video_time_us += m_impl->m_video_frame_duration_us;
            } else {
                rtc::binary audio_sample;
                // 音频这里用静音发生器做占位：固定 20ms 产出一次（与 Opus 常见帧长一致）。
                m_impl->m_audio_generator->produce_next_audio_sample(audio_sample);
                m_impl->m_sender->on_audio_sample(m_impl->m_next_audio_time_us, audio_sample);
                // 推进理想音频时间轴：同视频逻辑，保证 20ms 节拍稳定。
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

    // join 放在锁外：避免线程退出过程中若间接触发回调/再入需要拿 m_mutex 时造成死锁。
    if (loop_to_join.joinable()) loop_to_join.join();

    try {
        // 先停音频再停视频没有强依赖；这里保持与 start 顺序对称，保证资源尽快释放。
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


