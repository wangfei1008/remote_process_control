#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <memory>
#include <thread>
#include <vector>
#include <windows.h>
#include "capture_worker_compat.h"

// 前向声明，避免循环依赖
class remote_video_engine;
struct CapturedRawFrameWithTelemetry;
struct ResolveResult;
namespace rpc_video_contract { struct RawFrame; }

// ─────────────────────────────────────────────
// FrameRateTicker
// 封装帧率节拍控制，持有 next_tick 状态
// ─────────────────────────────────────────────
class FrameRateTicker {
public:
    explicit FrameRateTicker(int fps)
        : m_period(std::chrono::microseconds(1'000'000 / max(1, fps)))
        , m_next_tick(std::chrono::steady_clock::now())
    {}

    // 等待到下一帧时刻；若已超时则立即重置
    void wait_next() {
        m_next_tick += m_period;
        const auto now = std::chrono::steady_clock::now();
        if (m_next_tick > now) {
            std::this_thread::sleep_for(m_next_tick - now);
        } else {
            m_next_tick = now;
        }
    }

    // pipeline 未就绪时的短暂等待（不影响节拍状态）
    static void sleep_short() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

private:
    std::chrono::microseconds             m_period;
    std::chrono::steady_clock::time_point m_next_tick;
};

// ─────────────────────────────────────────────
// WindowMissingTracker
// 封装"窗口缺失 grace 期"的计时状态
// ─────────────────────────────────────────────
class WindowMissingTracker {
public:
    static constexpr uint64_t kGraceMs = 5000;

    // 记录首次发现缺失的时间戳
    void mark_missing(uint64_t now_unix_ms) {
        if (m_since_ms == 0) m_since_ms = now_unix_ms;
    }

    // grace 期是否已超时
    bool grace_expired(uint64_t now_unix_ms) const {
        return m_since_ms != 0 && (now_unix_ms - m_since_ms) >= kGraceMs;
    }

    // 检测到 surface 后重置
    void reset() { m_since_ms = 0; }

    bool is_missing() const { return m_since_ms != 0; }

private:
    uint64_t m_since_ms = 0;
};

// ─────────────────────────────────────────────
// CaptureWorker
//
// 生命周期：
//   1. 构造
//   2. start()  — 启动后台线程
//   3. stop()   — 请求停止（设标志）
//   4. join()   — 等待线程退出（析构自动调用）
//
// 注意：stop() + join() 必须在对象销毁前完成，
//       或者直接让析构函数处理（见 ~CaptureWorker）。
// ─────────────────────────────────────────────
class CaptureWorker {
public:
    explicit CaptureWorker(remote_video_engine& engine);

    // 禁止拷贝与移动（持有 std::thread，语义上不可复制）
    CaptureWorker(const CaptureWorker&)            = delete;
    CaptureWorker& operator=(const CaptureWorker&) = delete;
    CaptureWorker(CaptureWorker&&)                 = delete;
    CaptureWorker& operator=(CaptureWorker&&)      = delete;

    // 析构：若线程仍在运行则先 stop 再 join，防止 std::terminate
    ~CaptureWorker();

    // 启动后台线程；重复调用会抛出 std::logic_error
    void start();

    // 请求停止（仅设标志，不阻塞）
    void stop();

    // 阻塞等待线程退出；stop() 后调用
    void join();

    bool is_running() const { return m_thread.joinable(); }

private:
    struct Impl;

    // 线程入口，由 start() 在新线程中调用
    void run();

    //会话存活
    bool is_session_alive(const std::vector<DWORD>& pids) const;

    //Pipeline 就绪
    bool pipeline_ready() const;

    // 窗口解析
    // 返回 resolver 结果；副作用：可能 rebind capture_pid
    ResolveResult resolve_target(const std::vector<DWORD>& pids);

    //无 surface 处理 
    // 返回 true 表示已触发退出，调用方应 break
    bool handle_no_surfaces( const ResolveResult&  target, WindowMissingTracker& tracker, uint64_t now_unix_ms, bool session_alive);

    //帧抓取 
    std::optional<CapturedRawFrameWithTelemetry>
    grab_frame(const ResolveResult& target, uint64_t  now_unix_ms, uint64_t&   next_frame_id, bool filter_black, BmpDumpWriter&  bmp_dump);

    //帧分发（写共享槽 + 通知下游）
    void dispatch_frame(CapturedRawFrameWithTelemetry pkt);
private:
    remote_video_engine& m_engine;
    std::unique_ptr<Impl> m_impl;
    std::thread          m_thread;
    std::atomic<bool>    m_running{ false };
};
