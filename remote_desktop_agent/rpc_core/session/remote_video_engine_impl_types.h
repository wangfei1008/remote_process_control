#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

// 实现层（engine 私有）：线程同步容器、丢帧统计等。
// - 不承载跨模块语义；不应被 transport/receiver 等模块依赖
// - 可被 remote_video_engine 等 session 内部实现 include
//
// 注意：该文件当前不会被其他文件引用，仅作为结构拆分的落点。

namespace rpc_video_engine_impl {

// “最新值槽位”：生产者覆盖写，消费者取走。
template <class T>
struct LatestRawFrame {
    std::mutex mtx;
    std::condition_variable cv;
    std::optional<T> latest;
};

// 有界队列：push 时溢出则 drop oldest（drop 计数用于诊断）。
template <class T>
struct BoundedQueue {
    std::mutex mtx;
    std::deque<T> q;
    size_t max_size = 0; // 0 表示不限制
    uint64_t dropped = 0;

    void set_max_size(size_t n) { max_size = n; }

    void push_bounded(T&& v) {
        if (max_size > 0) {
            while (q.size() >= max_size) {
                q.pop_front();
                ++dropped;
            }
        }
        q.push_back(std::move(v));
    }
};

} // namespace rpc_video_engine_impl

