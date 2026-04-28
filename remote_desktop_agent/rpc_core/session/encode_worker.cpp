#include "session/encode_worker.h"

#include "session/remote_video_engine.h"

#include "common/rpc_time.h"
#include "encode/video_encode_pipeline.h"

#include <chrono>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include "capture_worker_compat.h"

EncodeWorker::EncodeWorker(remote_video_engine& engine)
    : m_engine(engine)
{}

EncodeWorker::~EncodeWorker()
{
    stop();
    join();
}

void EncodeWorker::start()
{
    if (m_thread.joinable()) {
        throw std::logic_error("EncodeWorker::start() called while already running");
    }
    if (!m_pipeline) {
        m_pipeline = std::make_unique<VideoEncodePipeline>();
        m_pipeline->configure(m_engine.m_video_fps);
    }
    m_pipeline->reset_for_stream_start();
    m_running.store(true, std::memory_order_release);    
    // 线程退出由 m_engine.m_threads_running 统一控制
    m_thread = std::thread(&EncodeWorker::run, this);
}

void EncodeWorker::stop()
{
    // 只请求停止：由 remote_video_engine::stop() 统一置 false 并唤醒 cv
    m_running.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_engine.m_latest_frame.mtx);
        m_engine.m_latest_frame.cv.notify_all();
    }
}

void EncodeWorker::join()
{
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void EncodeWorker::request_force_keyframe()
{
    if (!m_pipeline) return;
    try {
        m_pipeline->request_force_keyframe_with_cooldown(rpc_unix_epoch_ms());
    } catch (...) {
    }
}

void EncodeWorker::run()
{
    auto& e = m_engine;
    uint64_t last_encoded_id = 0;

    while (m_running.load(std::memory_order_acquire)) {
        std::optional<CapturedRawFrameWithTelemetry> packet;
        {
            std::unique_lock<std::mutex> lk(e.m_latest_frame.mtx);
            e.m_latest_frame.cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                return !m_running.load(std::memory_order_acquire) ||
                       (e.m_latest_frame.latest.has_value() &&
                        e.m_latest_frame.latest->frame.frame_id != last_encoded_id);
            });
            if (!m_running.load(std::memory_order_acquire)) break;
            if (!e.m_latest_frame.latest.has_value()) continue;
            if (e.m_latest_frame.latest->frame.frame_id == last_encoded_id) continue;

            packet = std::move(e.m_latest_frame.latest.value());
            e.m_latest_frame.latest.reset();
        }

        if (!packet.has_value() || !m_pipeline) {
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
            m_pipeline->ensure_encoder_layout(
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

        VideoEncodeResult enc = m_pipeline->encode_frame(*vec, captured_w, captured_h, applied_layout);

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
            request_force_keyframe();
        }

        release_raw_frame_owned(rf);
    }
}

