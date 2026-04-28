#pragma once

#include <memory>
#include <thread>

// 前向声明，避免循环依赖
class remote_video_engine;
class VideoEncodePipeline;

// 编码线程：从最新帧槽位取帧，编码后推入最新编码队列
class EncodeWorker {
public:
    explicit EncodeWorker(remote_video_engine& engine);

    EncodeWorker(const EncodeWorker&) = delete;
    EncodeWorker& operator=(const EncodeWorker&) = delete;
    EncodeWorker(EncodeWorker&&) = delete;
    EncodeWorker& operator=(EncodeWorker&&) = delete;

    ~EncodeWorker();

    void start();
    void stop();
    void join();

    void request_force_keyframe();

    bool is_running() const { return m_thread.joinable(); }

private:
    void run();

private:
    remote_video_engine& m_engine;
    std::unique_ptr<VideoEncodePipeline> m_pipeline;
    std::thread m_thread;
    std::atomic<bool>    m_running{ false };
};

