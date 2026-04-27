#include "session/remote_video_engine_workers.h"

#include "session/remote_video_engine.h"

#include "app/runtime_config.h"
#include "common/rpc_time.h"
#include "encode/video_encode_pipeline.h"
#include "input/input_controller.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <optional>
#include <thread>
#include <vector>

namespace {

void release_raw_frame_owned(rpc_video_contract::RawFrame& f)
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

BmpDumpDiag make_bmp_dump_diag_from_hw(bool used_hw)
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

namespace remote_video_engine_detail {

CaptureWorker::CaptureWorker(remote_video_engine& engine) : m_engine(engine) {}

void CaptureWorker::run()
{
    auto& e = m_engine;

    const int fps = (std::max)(1, e.m_video_fps);
    const auto frame_period = std::chrono::microseconds(1000000 / fps);
    auto next_tick = std::chrono::steady_clock::now();
    uint64_t next_frame_id = 1;
    const bool filter_black = runtime_config::get_bool("RPC_FILTER_CAPTURE_BLACK_FRAMES", true);

    uint64_t window_missing_since_unix_ms = 0;//第一次检测到窗口缺失的时间戳，用于判断是否超过 grace 时间阈值以触发远程退出通知。

    BmpDumpWriter bmp_dump;
    bmp_dump.configure_from_config();

    while (e.m_threads_running.load(std::memory_order_acquire)) {
        const auto now_unix_ms = rpc_unix_epoch_ms();

        if (!e.m_pipeline || !e.m_backend) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        std::cout << "[capture] 1 time = " << rpc_unix_epoch_ms() << std::endl;
        // Update process snapshots for watch thread & local health decisions.
        e.m_launch_pid_for_watch.store(e.m_session.launch_pid(), std::memory_order_release);
        e.m_capture_pid_for_watch.store(e.m_session.capture_pid(), std::memory_order_release);
        e.m_launch_running_for_watch.store(e.m_session.is_launch_running(), std::memory_order_release);
        std::cout << "[capture] 2 time = " << rpc_unix_epoch_ms() << std::endl;
        const HWND hwnd_hint = e.m_main_window.load(std::memory_order_acquire);
        const auto target = e.m_resolver.resolve({
            e.m_session.capture_pid(),
            e.m_session.launch_pid(),
            e.m_session.target_basename_lower(),
            hwnd_hint,
            true
        });
        std::cout << "[capture] 3 time = " << rpc_unix_epoch_ms() << std::endl;
        if (target.diag.pid_rebound) {
            e.m_session.rebind_capture_pid(target.capture_pid);
            e.m_capture_pid_for_watch.store(e.m_session.capture_pid(), std::memory_order_release);
        }

		std::cout << "[capture] time = " << now_unix_ms
                  << " resolver result for pid = " << e.m_session.capture_pid()
                  << " main_hwnd=" << static_cast<void*>(target.main_hwnd)
                  << " surfaces=" << target.surfaces.size()
			      << " lunch pid=" << e.m_session.launch_pid()
			<< std::endl;
        if (target.surfaces.empty()) {
            e.m_main_window.store(nullptr, std::memory_order_release);
            if (window_missing_since_unix_ms == 0) window_missing_since_unix_ms = now_unix_ms;

            if (now_unix_ms - window_missing_since_unix_ms >= 5000) {
                window_missing_since_unix_ms = 0;
                if (e.is_remote_process_still_running_from_snapshot()) {
                    e.notify_window_missing_if_needed("no_surfaces_grace_expired_but_process_alive", now_unix_ms);
                } else {
                    e.notify_remote_exit_if_needed("no_surfaces_grace_expired");
                    e.m_running = false;
                    e.m_threads_running.store(false, std::memory_order_release);
                }
            }

            const HWND hwnd_snapshot = e.m_main_window.load(std::memory_order_acquire);
            std::cout << "[capture] no surfaces found for pid=" << e.m_session.capture_pid()
                      << " main_window=" << static_cast<void*>(hwnd_snapshot)
                      << " resolver_why=" << (target.diag.reason ? target.diag.reason : "")
                      << " prev_pid=" << target.diag.previous_capture_pid
                      << " owner_pid=" << target.main_hwnd_owner_pid
                      << " pid_rebound=" << (target.diag.pid_rebound ? 1 : 0)
                      << " main_from_surfaces=" << (target.diag.selected_from_surfaces ? 1 : 0)
                      << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }
        std::cout << "[capture] 4 time = " << rpc_unix_epoch_ms() << std::endl;
        e.m_main_window.store(target.main_hwnd, std::memory_order_release);

        const uint64_t prep_unix_ms = rpc_unix_epoch_ms();
        rpc_video_contract::RawFrame rf;
        rpc_video_contract::TelemetrySnapshot telem;
        std::cout << "[capture] 5 time = " << rpc_unix_epoch_ms() << std::endl;
        if (e.m_pipeline->grab_raw_frame(target.surfaces, now_unix_ms, prep_unix_ms, next_frame_id, filter_black, rf, telem)) {
            auto* vec = static_cast<std::vector<uint8_t>*>(rf.owned.opaque);
            const int w = rf.coded_size.w;
            const int h = rf.coded_size.h;
            ++next_frame_id;

            input_controller::instance()->set_capture_screen_rect(rf.visible_rect.x, rf.visible_rect.y, w, h);

            const BmpDumpDiag bmp_diag =  make_bmp_dump_diag_from_hw(telem.backend == rpc_video_contract::CaptureBackend::Dxgi);
            bmp_dump.dump_capture_if_needed(*vec, w, h, bmp_diag);

            CapturedRawFrameWithTelemetry pkt;
            pkt.frame = std::move(rf);
            pkt.telem = telem;

            {
                std::lock_guard<std::mutex> lk(e.m_latest_frame.mtx);
                if (e.m_latest_frame.latest.has_value()) {
                    release_raw_frame_owned(e.m_latest_frame.latest->frame);
                }
                e.m_latest_frame.latest = std::move(pkt);
            }
            e.m_latest_frame.cv.notify_one();
        } else {
            std::cout << "[capture] grab_raw_frame failed for frame_id=" << next_frame_id << std::endl;
        }
        std::cout << "[capture] 6 time = " << rpc_unix_epoch_ms() << std::endl;
        next_tick += frame_period;
        const auto now = std::chrono::steady_clock::now();
        if (next_tick > now) {
            std::this_thread::sleep_for(next_tick - now);
        } else {
            next_tick = now;
        }
    }
}

EncodeWorker::EncodeWorker(remote_video_engine& engine) : m_engine(engine) {}

void EncodeWorker::run()
{
    auto& e = m_engine;
    uint64_t last_encoded_id = 0;

    while (e.m_threads_running.load(std::memory_order_acquire)) {
        std::optional<CapturedRawFrameWithTelemetry> packet;
        {
            std::unique_lock<std::mutex> lk(e.m_latest_frame.mtx);
            e.m_latest_frame.cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                return !e.m_threads_running.load(std::memory_order_acquire) ||
                       (e.m_latest_frame.latest.has_value() &&
                        e.m_latest_frame.latest->frame.frame_id != last_encoded_id);
            });
            if (!e.m_threads_running.load(std::memory_order_acquire)) break;
            if (!e.m_latest_frame.latest.has_value()) continue;
            if (e.m_latest_frame.latest->frame.frame_id == last_encoded_id) continue;

            packet = std::move(e.m_latest_frame.latest.value());
            e.m_latest_frame.latest.reset();
        }

        if (!packet.has_value() || !e.m_video_encode_pipeline) {
            if (packet.has_value()) release_raw_frame_owned(packet->frame);
            continue;
        }

        CapturedRawFrameWithTelemetry& pkt = packet.value();
        rpc_video_contract::RawFrame& rf = pkt.frame;
        last_encoded_id = rf.frame_id;

        const int captured_w = rf.coded_size.w;
        const int captured_h = rf.coded_size.h;
        int target_w = captured_w;
        int target_h = captured_h;
        bool applied_layout = false;

        const bool layout_ok =
            e.m_video_encode_pipeline->ensure_encoder_layout(
                captured_w, captured_h, target_w, target_h, applied_layout);
        if (!layout_ok) {
            std::cout << "[encode] unsupported frame layout: frame_id=" << rf.frame_id << std::endl;
            release_raw_frame_owned(rf);
            continue;
        }

        auto* vec = static_cast<std::vector<uint8_t>*>(rf.owned.opaque);
        if (!vec) {
            std::cout << "[encode] frame missing owned rgb buffer: frame_id=" << rf.frame_id << std::endl;
            release_raw_frame_owned(rf);
            continue;
        }

        VideoEncodeResult enc =
            e.m_video_encode_pipeline->encode_frame(*vec, captured_w, captured_h, applied_layout);

        if (enc.encode_ok && !enc.sample.empty() && !enc.invalid_payload) {
            EncodedFrameWithTelemetry out;
            out.payload_storage = std::move(enc.sample);
            out.telem = pkt.telem;
            out.telem.capture_size = rpc_video_contract::VideoSize{ target_w, target_h };
            out.telem.encode_unix_ms = enc.frame_unix_ms ? enc.frame_unix_ms : rpc_unix_epoch_ms();

            {
                std::lock_guard<std::mutex> lk(e.m_latest_encoded.mtx);
                e.m_latest_encoded.push_bounded(std::move(out));
            }
        } else {
            std::cout << "[encode] encode failed for frame_id=" << rf.frame_id << std::endl;
            e.request_force_keyframe();
        }

        release_raw_frame_owned(rf);
    }
}

} // namespace remote_video_engine_detail

