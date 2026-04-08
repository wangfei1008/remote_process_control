#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <vector>
#include "source/stream.h"
#include "capture/gdi_capture.h"
#include "capture/dxgi_capture.h"
#include "capture/bmp_dump_writer.h"
#include "session/remote_process_session.h"
#include "capture/capture_backend_state.h"
#include "encode/video_encode_pipeline.h"
#include "session/sample_output_state.h"
#include "policy/frame_pacing_policy.h"
#include <chrono>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <atomic>

class RemoteProcessStreamSource : public StreamSource
{
public:
    // 一次性快照遥测（线程安全）。
    // 用于 WebRTC 发送端在同一帧里构造 frameMark / captureHealth，避免多次 getter 分别加锁导致字段不一致。
    struct CaptureTelemetry {
        uint32_t last_capture_ms = 0;
        uint32_t last_encode_ms = 0;
        uint64_t last_frame_unix_ms = 0;

        bool last_capture_used_hw = false;
        bool dxgi_disabled_for_session = false;
        int top_black_strip_streak = 0;
        int dxgi_instability_score = 0;

        uint64_t force_software_capture_until_unix_ms = 0;
    };

    RemoteProcessStreamSource();
    ~RemoteProcessStreamSource();
    HWND launch_process(const std::string& exe_path);
    void terminate();

    // 当接收端（前端）检测到持续丢包/抖动时调用。
    // 强制下一帧视频包含 IDR 关键帧，以加快解码恢复。
    void request_force_keyframe();

    // 流源接口实现。
    void start() override;
    void stop() override;
    void load_next_sample() override;
    rtc::binary get_sample() override;
    uint64_t get_sample_time_us() override;
    uint64_t get_sample_duration_us() override;

    HWND get_main_window() const { return m_mainWindow; }
    //当前编码器实例使用的采集帧宽高。
    int get_capture_width() const { return m_capture_width_atomic.load(std::memory_order_relaxed); }
    int get_capture_height() const { return m_capture_height_atomic.load(std::memory_order_relaxed); }
    //最近一次采集耗时（毫秒）
    uint32_t get_last_capture_ms() const {
        std::lock_guard<std::mutex> lk(m_snapshot_mtx);
        return m_snapshot.last_capture_ms;
    }
    //最近一次 H.264 编码耗时（毫秒）
    uint32_t get_last_encode_ms() const {
        std::lock_guard<std::mutex> lk(m_snapshot_mtx);
        return m_snapshot.last_encode_ms;
    }
    //最近产生视频帧的 Unix 时间戳（毫秒）
    uint64_t get_last_frame_unix_ms() const {
        std::lock_guard<std::mutex> lk(m_snapshot_mtx);
        return m_snapshot.last_frame_unix_ms;
    }
    //最近一次采集是否使用了 DXGI 硬件复制
    bool get_last_capture_used_hw() const {
        std::lock_guard<std::mutex> lk(m_snapshot_mtx);
        return m_snapshot.last_capture_used_hw;
    }
    //当前会话是否因重复异常而禁用 DXGI
    bool is_dxgi_disabled_for_session() const {
        std::lock_guard<std::mutex> lk(m_snapshot_mtx);
        return m_snapshot.dxgi_disabled_for_session;
    }
    //顶部黑条异常模式的当前连续命中次数
    int get_top_black_strip_streak() const {
        std::lock_guard<std::mutex> lk(m_snapshot_mtx);
        return m_snapshot.top_black_strip_streak;
    }
    //会话级 DXGI 不稳定分数（越高表示失败越频繁）
    int get_dxgi_instability_score() const {
        std::lock_guard<std::mutex> lk(m_snapshot_mtx);
        return m_snapshot.dxgi_instability_score;
    }
    //强制软件采集截止到的 Unix 毫秒时间；0 表示未强制
    uint64_t get_force_software_capture_until_unix_ms() const {
        std::lock_guard<std::mutex> lk(m_snapshot_mtx);
        return m_snapshot.force_software_capture_until_unix_ms;
    }

    // 一次性获取遥测快照，减少多 getter 分别加锁。
    CaptureTelemetry get_capture_telemetry() const;

    //设置远程进程/窗口退出通知回调。
    void set_on_remote_exit(std::function<void()> cb) { m_on_remote_exit = std::move(cb); }

private:
    // 生产者-消费者：由后台线程生产“下一条样本”，
    // Stream 调用 load_next_sample() 时只做等待/接收。
    void producer_loop();

    void notify_remote_exit();
    //检查进程退出与 HWND 有效性；返回 false 表示采集前停止
    bool tick_window_and_health(std::chrono::steady_clock::time_point& last_no_window_diag);
    BmpDumpDiag make_dump_diag(bool use_hw_capture) const;
    void emit_hold_or_empty_sample(bool request_idr);
    bool decide_use_hw_capture(uint64_t cap_now_ms);
    bool grab_rgb_frame(int& width, int& height, int& cap_min_left, int& cap_min_top, std::vector<uint8_t>& frame,
                        bool use_hw_capture, bool& used_hw_capture_out);
    bool discard_if_capture_too_slow(std::chrono::steady_clock::time_point t_cap_begin,
                                    std::chrono::steady_clock::time_point t_after_cap, bool use_hw_capture);
    bool discard_if_empty_frame(const std::vector<uint8_t>& frame, int width, int height);
    bool filter_suspicious_frame(std::vector<uint8_t>& frame, int& width, int& height, int& cap_min_left,
                                 int& cap_min_top);
    void ensure_encoder_layout(int captured_w, int captured_h, bool& applied_layout_out);
    void update_fps_pacing_and_mapping(const std::vector<uint8_t>& frame, int captured_w, int captured_h,
                                       int cap_min_left, int cap_min_top);
    void finalize_encode_rgb(std::vector<uint8_t>& frame, int captured_w, int captured_h,
                             std::chrono::steady_clock::time_point t_cap_begin,
                             std::chrono::steady_clock::time_point t_after_cap, bool applied_layout);

    void update_snapshot_from_current_state();

    struct FrameSnapshot {
        int capture_width = 0;
        int capture_height = 0;
        uint32_t last_capture_ms = 0;
        uint32_t last_encode_ms = 0;
        uint64_t last_frame_unix_ms = 0;
        bool last_capture_used_hw = false;
        bool dxgi_disabled_for_session = false;
        int top_black_strip_streak = 0;
        int dxgi_instability_score = 0;
        uint64_t force_software_capture_until_unix_ms = 0;
    };

    PROCESS_INFORMATION m_pi;
    DWORD m_launchPid = 0;   // CreateProcess 返回的 PID
    DWORD m_capturePid = 0;  // 实际拥有被采集窗口的 PID
    HWND m_mainWindow = nullptr;
    std::string m_targetExeBaseName;
    int m_width;
    int m_height;
    int m_fps;
    int m_active_fps = 30;
    int m_idle_fps = 15;
    int m_idle_enter_stable_frames = 24;
    uint32_t m_recent_input_window_ms = 300;
    bool m_capture_all_windows = false;
    bool m_hw_capture_supported = false;
    bool m_hw_capture_requested = false;
    bool m_hw_capture_active = false;
    bool m_lock_capture_backend = false;
    bool m_locked_use_hw_capture = false;
    bool m_allow_pid_rebind_by_exename = true;
    uint64_t m_pid_rebind_deadline_unix_ms = 0;
    uint64_t m_window_missing_since_unix_ms = 0;
    uint32_t m_pid_rebind_startup_window_ms = 120000;
    uint32_t m_window_missing_exit_grace_ms = 5000;
    bool m_running = false;

    std::thread m_producer_thread;
    std::mutex m_producer_mtx;
    std::condition_variable m_producer_cv;
    // Stream 请求下一条样本时设置 m_need_produce=true，
    // 后台生产完置 m_has_produced=true 并唤醒等待方。
    bool m_need_produce = false;
    bool m_has_produced = false;
    GdiCapture m_gdiCapture; // GDI 窗口图像采集后端。
    DXGICapture m_dxgiCapture;
    SampleOutputState m_sample_output_state;
    // 保留最近一帧有效 RGB，用于轻量原位修复（例如顶部黑条）。
    std::vector<uint8_t> m_last_good_rgb_frame;
    int m_last_good_rgb_w = 0;
    int m_last_good_rgb_h = 0;
    uint32_t m_capture_degrade_ms = 2500;
    // 首帧前连续空帧计数；达到阈值时强制重选窗口，避免卡在不可采集句柄。
    uint32_t m_pre_video_empty_frame_streak = 0;
    uint32_t m_pre_video_reselect_empty_threshold = 30;

    std::function<void()> m_on_remote_exit;
    bool m_had_successful_video = false;
    bool m_exit_notified = false;

    BmpDumpWriter m_bmp_dump_writer;
    RemoteProcessSession m_remote_process_session;
    CaptureBackendState m_capture_backend_state;
    VideoEncodePipeline m_video_encode_pipeline;
    // 根据输入活跃度与帧变化情况动态调整采样帧率（降低空闲抖动与码率开销）。
    FramePacingPolicy m_frame_pacing_policy;

    FrameSnapshot m_snapshot;
    mutable std::mutex m_snapshot_mtx;

    // 为了避免在 on_sample 高频路径反复加锁：采集分辨率用原子读写。
    std::atomic<int> m_capture_width_atomic{0};
    std::atomic<int> m_capture_height_atomic{0};
};
