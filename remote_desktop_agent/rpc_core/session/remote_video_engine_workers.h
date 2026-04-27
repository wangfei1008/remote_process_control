#pragma once

class remote_video_engine;

namespace remote_video_engine_detail {

class CaptureWorker {
public:
    explicit CaptureWorker(remote_video_engine& engine);
    void run();

private:
    remote_video_engine& m_engine;
};

class EncodeWorker {
public:
    explicit EncodeWorker(remote_video_engine& engine);
    void run();

private:
    remote_video_engine& m_engine;
};

} // namespace remote_video_engine_detail

