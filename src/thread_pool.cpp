#include "minirt/thread_pool.h"

#include <stdexcept>
#include <iostream>

namespace minirt {

ThreadPool::ThreadPool(std::size_t threadCount)
{
    if (threadCount <= 0) {
        throw std::invalid_argument("ThreadPool threadCount must be greater than zero");
    }
    m_workers.reserve(threadCount); // 预分配内存

    /**
     * 在线程创建之前设置为 Running。
     * 
     * 工作线程一旦启动，就可能理解进入 WorkerLoop，因此不能等待所有线程创建完毕才修改状态。
     */
    m_state = RuntimeState::Running;

    try {
        for (std::size_t i = 0; i < threadCount; ++i) {
            m_workers.emplace_back(&ThreadPool::WorkerLoop, this);
        }
    } catch (...) {
        /*
         * 创建线程的过程中也可能会抛出异常。
         * 
         * 例如：系统资源不足时，std::thread 构造可能抛出 std::system_error
         * 
         * 此时，必须停止并回收已经创建好的线程
         */
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_state = RuntimeState::Stopping;
        }

        m_cv.notify_all();

        for (std::thread& worker : m_workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_state = RuntimeState::Stopped;
        }

        throw;
    }
}

ThreadPool::~ThreadPool()
{
    Shutdown();
}

void ThreadPool::Enqueue(Task task)
{
    if (task == nullptr) {
        throw std::invalid_argument("ThreadPool cannot enqueue an empty task");
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        /**
         * 只有 Running 状态可以接收新任务
         * 
         * Shutdown 和 Submit 竞争时：
         * 1. 如果 Submit 先获得锁，任务进入任务队列并被执行；
         * 2. 如果 Shutdown 先获得锁，Submit 被拒绝；
         */
        if (m_state != RuntimeState::Running) {
            throw std::runtime_error("Cannot submit task: ThreadPool is not running");
        }
        m_tasks.push(std::move(task));
    }

    /*
     * 先释放 mutex，再通知工作线程
     * 
     * 即使在持有锁时调用 notify_one 通常也不会导致错误，但被唤醒的线程仍然需要等待当前线程释放 mutex。
     */
    m_cv.notify_one();
}

void ThreadPool::Shutdown()
{
    ShutdownImpl(ShutdownMode::Drain);
}

void ThreadPool::ShutdownNow()
{
    ShutdownImpl(ShutdownMode::DiscardPending);
}

void ThreadPool::ShutdownImpl(ShutdownMode mode)
{
    /*
     * 防止多个线程同时执行 join。
     *
     * 第二个 Shutdown 调用会在这里等待第一个调用完成，随后发现状态已经是 Stopped，直接返回。
     */
    std::lock_guard<std::mutex> shutdownLock(m_shutdownMutex);

    /**
     * shutdown 丢弃的任务暂存在局部队列中
     * 
     * 先在 m_mutex 保护下与 m_tasks 进行交换，再在锁外销毁，避免在持有任务队列锁时执行大量析构操作
     */
    std::queue<Task> discardedTasks;
    {
        std::lock_guard<std::mutex> taskLock(m_mutex);
        
        if (m_state == RuntimeState::Stopped) {
            return;
        }
        m_state = RuntimeState::Stopping;
        
        if (mode == ShutdownMode::DiscardPending) {
            m_tasks.swap(discardedTasks);
        }
    }

    /**
     * 在任务队列锁外销毁被丢弃的任务
     * 
     * 被丢弃 packaged_task 的共享状态会进入 broken promise 状态，对应的 future.get() 将抛出 std::future_error。
     */
    {
        std::queue<Task> empty;
        discardedTasks.swap(empty);
    }

    /*
     * 所有休眠线程都需要醒来
     * 
     * 1. 队列中有任务的线程继续执行任务；
     * 2. 队列为空的线程检查通知条件并退出；
     */
    m_cv.notify_all();

    for (std::thread& worker : m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = RuntimeState::Stopped;
    }
}

RuntimeState ThreadPool::GetState() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

void ThreadPool::WorkerLoop()
{
    while (true) {
        Task task;
        {
            /**
             * 这里使用 unique_lock 是因为 condition_variable::wait() 在等待过程中需要：
             * 1. 自动释放 m_mutex
             * 2. 进入休眠
             * 3. 被唤醒后重新获取 mutex
             * 4. 返回调用代码
             * 
             * std::lock_guard 不支持中途主动结果和重新加锁，因此条件变量通常搭配 st::unique_lock。
             */
            std::unique_lock<std::mutex> lock(m_mutex);
            /*
             * 条件变量可能发生虚假唤醒，所以线程唤醒不代表一定存在任务，因此每次醒来都必须重新检查条件
             */
            m_cv.wait(lock, [this] {
                return m_state != RuntimeState::Running || !m_tasks.empty();
            });

            /*
             * 走到这里且队列为空，说明线程池已经进入 Stopping 状态。
             */
            if (m_tasks.empty()) {
                return;
            }

            task = std::move(m_tasks.front());
            m_tasks.pop();
        }

        /*
         * 必须在锁外执行任务，如果持有 mutex 执行任务：
         * 1. 其他工作线程无法获取任务；
         * 2. 提交线程也无法提交新任务；
         * 3. 整个线程池可能退化为串行执行；
         * 4. 任务中再次调用 Submit 时容易产生问题
         * 
         * 此外，这里继续保留最后一道异常保护。
         * 
         * 正常情况下，用户函数抛出的异常会被 packaged_task 捕获并写入 future，不会传播到这里。
         * 这里主要防御未来加入的内部任务包装器发生异常。
         */
        try {
            task();
        } catch (const std::exception& e) {
            std::cerr << "[MiniRuntime] internal task exception: " << e.what() << '\n';
        } catch (...) {
            std::cerr << "[MiniRuntime] unknown internal task exception\n";
        }
    }
}

} // namespace minirt
