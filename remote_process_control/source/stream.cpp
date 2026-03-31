#include "source/stream.h"
#include <ctime>
#include <winsock2.h> // for struct timeval
#ifdef _WIN32
// taken from https://stackoverflow.com/questions/5801813/c-usleep-is-obsolete-workarounds-for-windows-mingw
#include <windows.h>

void usleep(__int64 usec)
{
    HANDLE timer;
    LARGE_INTEGER ft;

    ft.QuadPart = -(10 * usec); // Convert to 100 nanosecond interval, negative value indicates relative time

    timer = CreateWaitableTimer(NULL, TRUE, NULL);
    SetWaitableTimer(timer, &ft, 0, NULL, NULL, 0);
    WaitForSingleObject(timer, INFINITE);
    CloseHandle(timer);
}

#else
#include <unistd.h>
#endif

#ifdef _MSC_VER
// taken from https://stackoverflow.com/questions/10905892/equivalent-of-gettimeday-for-windows
#include <windows.h>


struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

int gettimeofday(struct timeval* tv, struct timezone* tz) {
    if (tv) {
        FILETIME filetime; /* 64-bit value representing the number of 100-nanosecond intervals since
                              January 1, 1601 00:00 UTC */
        ULARGE_INTEGER x;
        ULONGLONG usec;
        static const ULONGLONG epoch_offset_us =
            11644473600000000ULL; /* microseconds betweeen Jan 1,1601 and Jan 1,1970 */

#if _WIN32_WINNT >= _WIN32_WINNT_WIN8
        GetSystemTimePreciseAsFileTime(&filetime);
#else
        GetSystemTimeAsFileTime(&filetime);
#endif
        x.LowPart = filetime.dwLowDateTime;
        x.HighPart = filetime.dwHighDateTime;
        usec = x.QuadPart / 10 - epoch_offset_us;
        tv->tv_sec = long(usec / 1000000ULL);
        tv->tv_usec = long(usec % 1000000ULL);
    }
    if (tz) {
        TIME_ZONE_INFORMATION timezone;
        GetTimeZoneInformation(&timezone);
        tz->tz_minuteswest = timezone.Bias;
        tz->tz_dsttime = 0;
    }
    return 0;
}
#else
#include <sys/time.h>
#endif

uint64_t current_time_in_microseconds() 
{
    struct timeval time;
    gettimeofday(&time, NULL);
    return uint64_t(time.tv_sec) * 1000 * 1000 + time.tv_usec;
}


Stream::Stream(std::shared_ptr<StreamSource> video, std::shared_ptr<StreamSource> audio) 
    : std::enable_shared_from_this<Stream>()
    , m_c_video(video)
    , m_c_audio(audio)
{
}

Stream::~Stream() 
{
    stop();
}

std::pair<std::shared_ptr<StreamSource>, Stream::StreamSourceType> Stream::unsafe_prepare_for_sample() 
{
    std::shared_ptr<StreamSource> ss;
    StreamSourceType sst;
    uint64_t nextTime;
    if (m_c_audio->get_sample_time_us() < m_c_video->get_sample_time_us()) {
        ss = m_c_audio;
        sst = StreamSourceType::Audio;
        nextTime = m_c_audio->get_sample_time_us();
    }
    else {
        ss = m_c_video;
        sst = StreamSourceType::Video;
        nextTime = m_c_video->get_sample_time_us();
    }

    auto currentTime = current_time_in_microseconds();

    auto elapsed = currentTime - m_start_time;
    // Waiting is handled outside the mutex by the caller.
    return { ss, sst };
}

void Stream::send_sample() {
    // Compute which source to send next and when, without holding the mutex during sleep.
    std::shared_ptr<StreamSource> ss;
    StreamSourceType sst;
    uint64_t nextTime = 0;
    uint64_t waitTime = 0;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_c_is_running) return;

        auto ssSST = unsafe_prepare_for_sample();
        ss = ssSST.first;
        sst = ssSST.second;
        nextTime = ss->get_sample_time_us();

        const auto currentTime = current_time_in_microseconds();
        const auto elapsed = currentTime - m_start_time;
        if (nextTime > elapsed) {
            waitTime = nextTime - elapsed;
        }
    }

    if (waitTime > 0) {
        usleep(waitTime);
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_c_is_running) return;
        auto sample = ss->get_sample();
        m_sample_handler(sst, ss->get_sample_time_us(), sample);
        ss->load_next_sample();
    }

    m_dispatch_queue.dispatch([this]() { this->send_sample(); });
}

void Stream::on_sample(std::function<void(StreamSourceType, uint64_t, rtc::binary)> handler)
{
    m_sample_handler = handler;
}

void Stream::start() 
{
    std::lock_guard lock(m_mutex);
    if (m_c_is_running) {
        return;
    }
    m_is_running = true;
    m_start_time = current_time_in_microseconds();
    m_c_audio->start();
    m_c_video->start();
    // Preload first samples so the very first sent frame isn't empty
    m_c_audio->load_next_sample();
    m_c_video->load_next_sample();
    m_dispatch_queue.dispatch([this]() {
        this->send_sample();
        });
}

void Stream::stop() 
{
    std::lock_guard lock(m_mutex);
    if (!m_c_is_running) return;
    m_is_running = false;
    m_dispatch_queue.remove_pending();
    m_c_audio->stop();
    m_c_video->stop();
}