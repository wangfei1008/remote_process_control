/**
 * 基于 libdatachannel 的推流示例
 * 版权所有 (c) 2020 Filip Klembara (in2core)
 *
 * 本源码受 Mozilla Public License v2.0 约束。
 * 若未随文件附带 MPL 副本，可在 https://mozilla.org/MPL/2.0/ 获取。
 */


#include "transport/dispatch_queue.hpp"

DispatchQueue::DispatchQueue(std::string name, size_t threadCount) :name{std::move(name)}, threads(threadCount) 
{
    for(size_t i = 0; i < threads.size(); i++)
    {
        threads[i] = std::thread(&DispatchQueue::dispatch_thread_handler, this);
    }
}

DispatchQueue::~DispatchQueue() {
    // 通知分发线程进入收尾阶段
    std::unique_lock<std::mutex> lock(lockMutex);
    quit = true;
    lock.unlock();
    condition.notify_all();

    // 退出前等待线程全部结束
    for(size_t i = 0; i < threads.size(); i++)
    {
        if(threads[i].joinable())
        {
            threads[i].join();
        }
    }
}

void DispatchQueue::remove_pending() {
    std::unique_lock<std::mutex> lock(lockMutex);
    queue = {};
}

void DispatchQueue::dispatch(const fp_t& op) {
    std::unique_lock<std::mutex> lock(lockMutex);
    queue.push(op);

    // 通知前先手动解锁，避免唤醒线程后再次阻塞
    // 详见 notify_one 的行为说明
    lock.unlock();
    condition.notify_one();
}

void DispatchQueue::dispatch(fp_t&& op) {
    std::unique_lock<std::mutex> lock(lockMutex);
    queue.push(std::move(op));

    // 通知前先手动解锁，避免唤醒线程后再次阻塞
    // 详见 notify_one 的行为说明
    lock.unlock();
    condition.notify_one();
}

void DispatchQueue::dispatch_thread_handler(void) 
{
    std::unique_lock<std::mutex> lock(lockMutex);
    do {
        // 等待直到有任务或收到退出信号
        condition.wait(lock, [this]{
            return (queue.size() || quit);
        });

        // 等待返回后当前线程持有锁
        if(!quit && queue.size())
        {
            auto op = std::move(queue.front());
            queue.pop();

            // 队列操作完成后先解锁
            lock.unlock();

            op();

            lock.lock();
        }
    } while (!quit);
}
