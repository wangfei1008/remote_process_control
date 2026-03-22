#pragma once
#include "dispatch_queue.hpp"
#include "rtc/rtc.hpp"

class StreamSource 
{
protected:

public:
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void load_next_sample() = 0;

    virtual uint64_t get_sample_time_us() = 0;
    virtual uint64_t get_sample_duration_us() = 0;
    virtual rtc::binary get_sample() = 0;
};

class Stream : public std::enable_shared_from_this<Stream> 
{
public:
    enum class StreamSourceType {
        Audio,
        Video
    };

    Stream(std::shared_ptr<StreamSource> video, std::shared_ptr<StreamSource> audio);
    ~Stream();
    void on_sample(std::function<void(StreamSourceType, uint64_t, rtc::binary)> handler);
    void start();
    void stop();

private:  
    std::pair<std::shared_ptr<StreamSource>, StreamSourceType> unsafe_prepare_for_sample();
    void send_sample();

public:
    const bool& m_c_is_running = m_is_running;
    const std::shared_ptr<StreamSource> m_c_audio;
    const std::shared_ptr<StreamSource> m_c_video;
private:
    rtc::synchronized_callback<StreamSourceType, uint64_t, rtc::binary> m_sample_handler;
    uint64_t m_start_time = 0;
    std::mutex m_mutex;
    DispatchQueue m_dispatch_queue = DispatchQueue("StreamQueue");
    bool m_is_running = false;
};


