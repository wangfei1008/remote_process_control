/**
 * 基于 libdatachannel 的推流示例
 * 版权所有 (c) 2020 Filip Klembara (in2core)
 *
 * 本源码受 Mozilla Public License v2.0 约束。
 * 若未随文件附带 MPL 副本，可在 https://mozilla.org/MPL/2.0/ 获取。
 */

#ifndef dispatchqueue_hpp
#define dispatchqueue_hpp

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <string>

class DispatchQueue {
    typedef std::function<void(void)> fp_t;

public:
    DispatchQueue(std::string name, size_t threadCount = 1);
    ~DispatchQueue();

    // 分发任务（拷贝）
    void dispatch(const fp_t& op);
    // 分发任务（移动）
    void dispatch(fp_t&& op);

    void remove_pending();

    // 禁用拷贝/移动语义
    DispatchQueue(const DispatchQueue& rhs) = delete;
    DispatchQueue& operator=(const DispatchQueue& rhs) = delete;
    DispatchQueue(DispatchQueue&& rhs) = delete;
    DispatchQueue& operator=(DispatchQueue&& rhs) = delete;

private:
    std::string name;
    std::mutex lockMutex;
    std::vector<std::thread> threads;
    std::queue<fp_t> queue;
    std::condition_variable condition;
    bool quit = false;

    void dispatch_thread_handler(void);
};

#endif /* dispatchqueue_hpp */
