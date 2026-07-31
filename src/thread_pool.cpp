#include "minirt/thread_pool.h"

#include <stdexcept>
#include <iostream>

namespace minirt {

thread_local ThreadPool* ThreadPool::m_currentPool = nullptr;

ThreadPool::ThreadPool(std::size_t threadCount)
    : ThreadPool(ThreadPoolOptions { threadCount, 1024, RejectionPolicy::Block }) {}

ThreadPool::ThreadPool(ThreadPoolOptions options) : m_options(options)
{
    if (m_options.threadCount <= 0) {
        throw std::invalid_argument("ThreadPool threadCount must be greater than zero");
    }

    if (m_options.queueCapacity == 0) {
        throw std::invalid_argument("ThreadPool queueCapacity must be greater than zero");
    }

    m_workers.reserve(m_options.threadCount); // 预分配内存

    /**
     * 在线程创建之前设置为 Running。
     * 
     * 工作线程一旦启动，就可能理解进入 WorkerLoop，因此不能等待所有线程创建完毕才修改状态。
     */
    m_state = RuntimeState::Running;

    try {
        for (std::size_t i = 0; i < m_options.threadCount; ++i) {
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

        m_taskAvailableCv.notify_all();
        m_queueNotFullCv.notify_all();

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

void ThreadPool::Dispatch(Task task)
{
    if (task == nullptr) {
        throw std::invalid_argument("ThreadPool cannot dispatch an empty task");
    }

    bool runInCaller = false;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        /**
         * 只有 Running 状态可以接收新任务
         * 
         * Shutdown 和 Submit 竞争时：
         * 1. 如果 Submit 先获得锁，任务进入任务队列并被执行；
         * 2. 如果 Shutdown 先获得锁，Submit 被拒绝；
         */
        if (m_state != RuntimeState::Running) {
            m_metrics.OnRejected();
            throw std::runtime_error("Cannot submit task: ThreadPool is not running");
        }

        switch (m_options.rejectionPolicy) {
            case RejectionPolicy::Block: {
                /*
                * 如果当前就是本线程的工作线程并且队列已满，继续阻塞可能产生递归提交，出现死锁
                * 此时退化为 CallerRuns，在当前工作线程中执行
                */
                if (m_tasks.size() >= m_options.queueCapacity && m_currentPool == this) {
                    runInCaller = true;
                } else {
                    m_queueNotFullCv.wait(lock, [this] {
                        return m_state != RuntimeState::Running || m_tasks.size() < m_options.queueCapacity;
                    });

                    if (m_state != RuntimeState::Running) {
                        m_metrics.OnRejected();
                        throw TaskRejected("ThreadPool stopped while submitter was waiting for queue space");
                    }
                }
                break;
            }

            case RejectionPolicy::Reject: {
                if (m_tasks.size() >= m_options.queueCapacity) {
                    m_metrics.OnRejected();
                    throw TaskRejected("Cannot submit task: task queue is full");
                }
                break;
            }

            case RejectionPolicy::CallerRuns: {
                if (m_tasks.size() >= m_options.queueCapacity) {
                    runInCaller = true;
                }
                break;
            }
        
            default:
                break;
        }

        if (runInCaller) {
            // CallerRuns 任务虽然不在工作线程中运行，仍然属于线程池已经接受的任务。
            ++m_activeTasks;
            m_metrics.OnSubmitted();
            m_metrics.OnCallerRuns();
        } else {
            m_tasks.push(std::move(task));
            m_metrics.OnSubmitted();
        }
    }

    // 必须在 m_mutex 外执行。
    if (runInCaller) {
        ThreadPool* previousPool = m_currentPool;
        m_currentPool = this;

        try {
            task();
        } catch (const std::exception& exception) {
            std::cerr << "[MiniRuntime] internal task exception: " << exception.what()<< '\n';
        } catch (...) {
            std::cerr << "[MiniRuntime] unknown internal task exception\n";
        }

        m_currentPool = previousPool;
        FinishTaskExecution();
        return;
    } 

    /*
     * 先释放 mutex，再通知工作线程
     * 
     * 即使在持有锁时调用 notify_one 通常也不会导致错误，但被唤醒的线程仍然需要等待当前线程释放 mutex。
     */
    m_taskAvailableCv.notify_one();
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
    if (m_currentPool == this) {
        throw std::logic_error("Shutdown cannot be called from a task running in this ThreadPool");
    }

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
            const std::size_t discardedCount = m_tasks.size();
            m_tasks.swap(discardedTasks);
            m_metrics.OnDiscarded(discardedCount);
        }
    }

    /*
     * 
     * 同时唤醒：
     * 
     * 1. 等待任务的工作线程
     * 2. 等待队列空间的 Block 提交线程
     */
    m_taskAvailableCv.notify_all();
    m_queueNotFullCv.notify_all();

    /**
     * 在任务队列锁外销毁被丢弃的任务
     * 
     * 被丢弃 packaged_task 的共享状态会进入 broken promise 状态，对应的 future.get() 将抛出 std::future_error。
     */
    {
        std::queue<Task> empty;
        discardedTasks.swap(empty);
    }

    for (std::thread& worker : m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_idleCv.wait(lock, [this] {
            return m_activeTasks == 0;
        });
        m_state = RuntimeState::Stopped;
    }
}

RuntimeState ThreadPool::GetState() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

std::size_t ThreadPool::PendingTaskCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_tasks.size();
}

ThreadPoolOptions ThreadPool::GetOptions() const noexcept
{
    return m_options;
}

RuntimeMetricsSnapshot ThreadPool::GetMetrics() const noexcept
{
    return m_metrics.GetSnapshot();
}

void ThreadPool::WorkerLoop()
{
    m_currentPool = this;

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
            m_taskAvailableCv.wait(lock, [this] {
                return m_state != RuntimeState::Running || !m_tasks.empty();
            });

            /*
             * 走到这里且队列为空，说明线程池已经进入 Stopping 状态。
             */
            if (m_tasks.empty()) {
                m_currentPool = nullptr;
                return;
            }

            task = std::move(m_tasks.front());
            m_tasks.pop();

            ++m_activeTasks;
        }

        // 从队列取走一个任务后，队列出现新空间。
        m_queueNotFullCv.notify_one();

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

        FinishTaskExecution();
    }
}

void ThreadPool::FinishTaskExecution() noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_activeTasks > 0) {
        m_activeTasks--;
    }

    if (m_activeTasks == 0 && m_tasks.empty()) {
        m_idleCv.notify_all();
    }
}

} // namespace minirt
