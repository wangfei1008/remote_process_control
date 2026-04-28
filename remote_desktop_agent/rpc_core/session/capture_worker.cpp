#include "capture_worker.h"

#include <algorithm>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <memory>

#include "session/remote_video_engine.h"

#include "app/runtime_config.h"
#include "common/rpc_time.h"
#include "encode/video_encode_pipeline.h"
#include "input/input_controller.h"

#include "capture/policy/window_score_policy.h"
#include "capture/policy/capture_target_resolver.h"
#include "capture/infra/win32_process.h"
#include "capture/infra/win32_window.h"
#include "capture/backend/capture_backend_factory.h"
#include "capture/backend/i_capture_backend.h"
#include "capture/pipeline/capture_pipeline.h"
#include "capture/pipeline/frame_composer.h"
#include "capture/pipeline/frame_filter.h"



#define LOG_THROTTLE(interval_ms, expr)                     \
    do {                                                    \
        static uint64_t _last_ms = 0;                      \
        const uint64_t  _now_ms  = rpc_unix_epoch_ms();    \
        if (_now_ms - _last_ms >= (interval_ms)) {         \
            _last_ms = _now_ms;                            \
            expr;                                          \
        }                                                  \
    } while (0)



struct CaptureWorker::Impl {
    win32::Window wops{};
    win32::Process prims{};
    capture::WindowScorePolicy score_policy{};
    capture::CaptureTargetResolver resolver{
        capture::CaptureTargetResolver::Deps{ wops, prims, score_policy }
    };

    std::unique_ptr<capture::ICaptureBackend> backend{};
    capture::FrameComposer composer{};
    capture::FrameFilter filter{};
    std::unique_ptr<capture::CapturePipeline> pipeline{};

    void ensure_capture_stack()
    {
        if (backend && pipeline) return;
        pipeline.reset();
        backend = capture::create_capture_backend_from_config();
        if (backend) {
            std::cout << "[capture] capture_kind=" << static_cast<int>(backend->kind()) << "\n";
            backend->on_new_session();
        }
        if (!backend) return;
        pipeline = std::make_unique<capture::CapturePipeline>(capture::CapturePipeline::Deps{ *backend, composer, filter });
    }
};

CaptureWorker::CaptureWorker(remote_video_engine& engine)
    : m_engine(engine)
    , m_impl(std::make_unique<Impl>())
{}

CaptureWorker::~CaptureWorker()
{
    // 防止线程仍在跑时对象被销毁 → std::terminate
    stop();
    join();
}


// 线程生命周期管理
void CaptureWorker::start()
{
    if (m_thread.joinable()) {
        throw std::logic_error("CaptureWorker::start() called while already running");
    }
    // 先置标志再启动线程，避免线程起来就因标志为 false 退出
    m_running.store(true, std::memory_order_release);
    m_thread = std::thread(&CaptureWorker::run, this);
}

void CaptureWorker::stop()
{
    m_running.store(false, std::memory_order_release);
}

void CaptureWorker::join()
{
    if (m_thread.joinable()) {
        m_thread.join();
    }
}


// run() —— 主循环，只描述"发生了什么"
void CaptureWorker::run()
{
    auto& e = m_engine;

    FrameRateTicker      ticker(e.m_video_fps);
    WindowMissingTracker win_tracker;
    uint64_t             next_frame_id = 1;

    const bool filter_black = runtime_config::get_bool("RPC_FILTER_CAPTURE_BLACK_FRAMES", true);

    BmpDumpWriter bmp_dump;
    bmp_dump.configure_from_config();

    while (m_running.load(std::memory_order_acquire))
    {
        const uint64_t           now_ms = rpc_unix_epoch_ms();
        const std::vector<DWORD> pids   = e.m_session.session_pids();

        // 1. 会话存活检查 
        if (!is_session_alive(pids)) {
            e.notify_remote_exit_if_needed("session_not_alive");
            m_running.store(false, std::memory_order_release);
            break;
        }

        // 2. Pipeline 就绪检查
        m_impl->ensure_capture_stack();
        if (!pipeline_ready()) {
            FrameRateTicker::sleep_short();
            continue;
        }

        //3. 窗口解析 
        const ResolveResult target = resolve_target(pids);

        // 4. 无 surface 处理
        if (target.surfaces.empty()) {
            const bool should_exit = handle_no_surfaces(target, win_tracker, now_ms, true);
            if (should_exit) break;
            continue;
        }

        // surface 有效，重置缺失计时器
        win_tracker.reset();
        e.m_main_window.store(target.main_hwnd, std::memory_order_release);

        //5. 帧抓取与分发
        if (auto pkt = grab_frame(target, now_ms, next_frame_id, filter_black, bmp_dump)) {
            dispatch_frame(std::move(*pkt));
        }

        //6. 帧率节拍等待
        ticker.wait_next();
    }
}


//1. 会话存活 
bool CaptureWorker::is_session_alive(const std::vector<DWORD>& pids) const
{
    auto& s = m_engine.m_session;

    bool alive = s.is_session_alive_with_pids(pids.data(), pids.size());

    // 有 Job 时：PID 列表偶发为空，但 launch 句柄仍存活 ⇒ 会话未结束
    if (!alive && s.has_job_identity() && s.is_launch_running()) {
        alive = true;
    }

    // 兜底：Job 列表误判/竞态，capture_pid 仍在运行
    if (!alive) {
        const DWORD cap = s.capture_pid();
        if (cap != 0 && m_impl->prims.is_running(cap)) {
            alive = true;
        }
    }

    return alive;
}


//2. Pipeline 就绪
bool CaptureWorker::pipeline_ready() const
{
    return m_impl->pipeline && m_impl->backend;
}


//3. 窗口解析 
ResolveResult CaptureWorker::resolve_target(const std::vector<DWORD>& pids)
{
    auto& e = m_engine;
    const HWND hwnd_hint = e.m_main_window.load(std::memory_order_acquire);

    const ResolveResult target = m_impl->resolver.resolve({
        e.m_session.capture_pid(),
        e.m_session.launch_pid(),
        e.m_session.target_basename_lower(),
        hwnd_hint,
        true,
        std::span<const DWORD>(pids.data(), pids.size()),
        true
    });

    if (target.diag.pid_rebound) {
        e.m_session.rebind_capture_pid(target.capture_pid);
    }

    LOG_THROTTLE(1000,
        std::cout << "[capture] resolver"
                  << " pid="       << e.m_session.capture_pid()
                  << " main_hwnd=" << static_cast<void*>(target.main_hwnd)
                  << " surfaces="  << target.surfaces.size()
                  << " launch_pid="<< e.m_session.launch_pid()
                  << std::endl
    );

    return target;
}


// 4. 无 surface 处理 
//
// 返回 true  → 调用方应 break（触发了退出流程）
// 返回 false → 调用方应 continue（继续等待）
bool CaptureWorker::handle_no_surfaces( const ResolveResult&  target, WindowMissingTracker& tracker, uint64_t now_unix_ms, bool session_alive)
{
    auto& e = m_engine;

    e.m_main_window.store(nullptr, std::memory_order_release);
    tracker.mark_missing(now_unix_ms);

    LOG_THROTTLE(2000,
        std::cout << "[capture] no surfaces"
                  << " pid="          << e.m_session.capture_pid()
                  << " main_window="  << static_cast<void*>(
                                           e.m_main_window.load(std::memory_order_acquire))
                  << " why="          << (target.diag.reason ? target.diag.reason : "")
                  << " prev_pid="     << target.diag.previous_capture_pid
                  << " owner_pid="    << target.main_hwnd_owner_pid
                  << " pid_rebound="  << (target.diag.pid_rebound ? 1 : 0)
                  << " from_surfaces="<< (target.diag.selected_from_surfaces ? 1 : 0)
                  << std::endl
    );

    if (tracker.grace_expired(now_unix_ms)) {
        tracker.reset();  // 重置，防止下轮立即重复触发
        if (session_alive) {
            // 进程仍存活，通知上层但不退出，继续等待窗口出现
            e.notify_window_missing_if_needed(
                "no_surfaces_grace_expired_but_process_alive", now_unix_ms);
        } else {
            e.notify_remote_exit_if_needed("no_surfaces_grace_expired");
            m_running.store(false, std::memory_order_release);
            return true;  // 触发退出
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    return false;
}


// 5a. 帧抓取 
std::optional<CapturedRawFrameWithTelemetry>
CaptureWorker::grab_frame( const ResolveResult& target, uint64_t now_unix_ms, uint64_t& next_frame_id, bool filter_black,BmpDumpWriter& bmp_dump)
{
    auto& e = m_engine;

    const uint64_t prep_unix_ms = rpc_unix_epoch_ms();
    rpc_video_contract::RawFrame          rf;
    rpc_video_contract::TelemetrySnapshot telem;

    if (!m_impl->pipeline->grab_raw_frame(target.surfaces, now_unix_ms, prep_unix_ms, next_frame_id, filter_black, rf, telem))
    {
        std::cout << "[capture] grab_raw_frame failed"
                  << " frame_id=" << next_frame_id << std::endl;
        return std::nullopt;
    }

    ++next_frame_id;

    const int w = rf.coded_size.w;
    const int h = rf.coded_size.h;
    input_controller::instance()->set_capture_screen_rect(
        rf.visible_rect.x, rf.visible_rect.y, w, h);

    const BmpDumpDiag bmp_diag = make_bmp_dump_diag_from_hw( telem.backend == rpc_video_contract::CaptureBackend::Dxgi);
    auto* vec = static_cast<std::vector<uint8_t>*>(rf.owned.opaque);
    bmp_dump.dump_capture_if_needed(*vec, w, h, bmp_diag);

    CapturedRawFrameWithTelemetry pkt;
    pkt.frame = std::move(rf);
    pkt.telem = telem;
    return pkt;
}


//  5b. 帧分发 
//
// 锁粒度优化：
//   锁内只做"交换"（移动语义，纳秒级），
//   锁外再释放旧帧（可能触发系统调用），最小化持锁时间。
void CaptureWorker::dispatch_frame(CapturedRawFrameWithTelemetry pkt)
{
    auto& slot = m_engine.m_latest_frame;

    std::optional<CapturedRawFrameWithTelemetry> old_pkt;
    {
        std::lock_guard<std::mutex> lk(slot.mtx);
        if (slot.latest.has_value()) {
            old_pkt = std::move(slot.latest);  // 锁内只做移动
        }
        slot.latest = std::move(pkt);
    }
    // 锁外释放旧帧，避免持锁期间执行可能阻塞的资源释放
    if (old_pkt.has_value()) {
        release_raw_frame_owned(old_pkt->frame);
    }

    slot.cv.notify_one();
}
