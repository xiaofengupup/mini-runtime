/**
 * 基础线程池
 */
#pragma once

#include <vector>
#include <thread>
#include <functional>
#include <queue>
#include <mutex>
#include <condition_variable>

namespace minirt {

class ThreadPool {
public:
    using Task = std::function<void()>;

    /**
     * 创建固定数量工作线程的线程池
     * 
     * @param threadCount 工作线程数量，必须大于 0
     */
    explicit ThreadPool(std::size_t threadCount);

    /**
     * 析构时停止线程池，并等待所有工作线程退出
     */
    ~ThreadPool();

    // 禁止拷贝和移动
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /**
     * 提交一个无参数、无返回值的任务·
     * 
     * 如果线程池已经停止，则抛出异常
     */
    void Submit(Task task);

    /**
     * 停止接收新任务
     * 
     * 已经进入任务队列的任务会继续执行；
     * 该函数会等待所有工作线程退出，可以重复调用；
     */
    void Stop();

private:
    /**
     * 工作线程执行函数
     */
    void WorkerLoop();

private:
    std::vector<std::thread> m_workers;
    std::queue<Task> m_tasks;

    std::mutex m_mutex;
    std::condition_variable m_cv;

    bool m_stopping {false};

    /**
     * std::once_flag 是 C++ 11 引入的一个轻量级同步原语，位于 <mutex> 文件中。
     * 
     * 主要用途：确保某段代码在整个程序的生命周期中只被执行一次。通常与 std::call_once 函数配合使用。
     * 
     * 核心特性：
     *    1. 不可复制、不可移动；
     *    2. 非透明状态：无法通过 std::once_flag 的内部状态来判断它是否已经被执行过，只能通过 std::call_once 来触发
     *    3. 线程安全：在多线程环境下，std::call_once 能保证即使有多个线程同时尝试执行，也只有一个线程会真正执行目标函数，
     *                其它线程会被阻塞直到该函数执行完毕。
     * 
     * 此处用来保证 Stop() 中的停止和 join 逻辑只执行一次!
     */
    std::once_flag m_stopOnce;
};

} // namespace minirt