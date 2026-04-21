#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

// 实现层（engine 私有）：线程同步容器、丢帧统计等。
// - 不承载跨模块语义；不应被 transport/receiver 等模块依赖
// - 可被 remote_video_engine 等 session 内部实现 include
//
// 注意：该文件当前不会被其他文件引用，仅作为结构拆分的落点。

namespace rpc_video_engine_impl {

// “最新值槽位”：生产者覆盖写，消费者取走（overwrite 计数用于诊断）。
template <class T>
struct LatestSlot {
    std::mutex mtx;
    std::condition_variable cv;
    std::optional<T> latest;

    uint64_t dropped_by_overwrite = 0;
    uint64_t stored_items = 0;
};

// 有界队列：push 时溢出则 drop oldest（drop 计数用于诊断）。
template <class T>
struct BoundedQueue {
    std::mutex mtx;
    std::deque<T> q;
    size_t capacity = 1;

    uint64_t dropped_by_overflow = 0;
    uint64_t pushed = 0;
};

} // namespace rpc_video_engine_impl

