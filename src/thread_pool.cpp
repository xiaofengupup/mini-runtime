#include "minirt/thread_pool.h"

#include <stdexcept>
#include <iostream>

namespace minirt {

thread_local ThreadPool* ThreadPool::m_currentPool = nullptr;
thread_local std::size_t ThreadPool::m_currentWorkerIndex = ThreadPool::m_kNoWorker;

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

    /**
     * m_localQueues 中的对象包含 mutex，因此通过 unique_ptr 保存，避免 vector 扩容时要求队列可移动
     */
    m_localQueues.reserve(m_options.threadCount);
    for (std::size_t i = 0; i < m_options.threadCount; ++i) {
        m_localQueues.push_back(std::make_unique<WorkStealingQueue<Task>>());
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
            m_workers.emplace_back(&ThreadPool::WorkerLoop, this, i);
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
            m_discardPending.store(true, std::memory_order_release);
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
    bool notifyWorker = false;
    const bool isWorkerThread = (m_currentPool == this && m_currentWorkerIndex != m_kNoWorker);
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
            throw TaskRejected("Cannot submit task: ThreadPool is not running");
        }

        if (isWorkerThread) {
            /*
             * 工作线程内部提交
             *
             * 单工作线程中，如果子任务入队，当前任务随后等待子任务 future，很容易产生自我死锁。
             * 因此单线程池内部提交直接执行。
             */
            if (m_options.threadCount == 1) {
                runInCaller = true;
                ++m_activeTasks;

                m_metrics.OnSubmitted();
                m_metrics.OnCallerRuns();
            } else {
                m_localQueues[m_currentWorkerIndex]->Push(std::move(task));
                m_pendingTasks.fetch_add(1, std::memory_order_release);

                m_metrics.OnSubmitted();
                m_metrics.OnLocalSubmitted();

                notifyWorker = true;
            }
        } else {
            // 外部线程提交，仍然使用全局有界队列和 Day 3 的拒绝策略。
            switch (m_options.rejectionPolicy) {
                case RejectionPolicy::Block: {
                    /*
                    * 如果当前就是本线程的工作线程并且队列已满，继续阻塞可能产生递归提交，出现死锁
                    * 此时退化为 CallerRuns，在当前工作线程中执行
                    */
                    if (m_globalTasks.size() >= m_options.queueCapacity && m_currentPool == this) {
                        runInCaller = true;
                    } else {
                        m_queueNotFullCv.wait(lock, [this] {
                            return m_state != RuntimeState::Running || m_globalTasks.size() < m_options.queueCapacity;
                        });

                        if (m_state != RuntimeState::Running) {
                            m_metrics.OnRejected();
                            throw TaskRejected("ThreadPool stopped while submitter was waiting for queue space");
                        }
                    }
                    break;
                }

                case RejectionPolicy::Reject: {
                    if (m_globalTasks.size() >= m_options.queueCapacity) {
                        m_metrics.OnRejected();
                        throw TaskRejected("Cannot submit task: task queue is full");
                    }
                    break;
                }

                case RejectionPolicy::CallerRuns: {
                    if (m_globalTasks.size() >= m_options.queueCapacity) {
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
                m_globalTasks.push(std::move(task));

                m_pendingTasks.fetch_add(1, std::memory_order_release);
                m_metrics.OnSubmitted();
                notifyWorker = true;
            }
        }
    }

    // 必须在 m_mutex 外执行。
    if (runInCaller) {
        ThreadPool* previousPool = m_currentPool;
        const std::size_t previousWorkIndex = m_currentWorkerIndex;

        m_currentPool = this;
        m_currentWorkerIndex = m_kNoWorker;

        try {
            task();
        } catch (const std::exception& exception) {
            std::cerr << "[MiniRuntime] internal task exception: " << exception.what()<< '\n';
        } catch (...) {
            std::cerr << "[MiniRuntime] unknown internal task exception\n";
        }

        m_currentPool = previousPool;
        m_currentWorkerIndex = previousWorkIndex;
        FinishTaskExecution();
        return;
    } 

    /*
     * 先释放 mutex，再通知工作线程
     * 
     * 即使在持有锁时调用 notify_one 通常也不会导致错误，但被唤醒的线程仍然需要等待当前线程释放 mutex。
     */
    if (notifyWorker) {
        m_taskAvailableCv.notify_one();
    }
}

void ThreadPool::DispatchOnly(Task task)
{
    if (task == nullptr) {
        throw std::invalid_argument("ThreadPool cannot dispatch an empty task");
    }

    /**
     * fire-and-forget 任务特点：
     *  - 不等待：调用线程不会等待任务执行结果。
     *  - 非阻塞提交：队列满时直接拒绝，不执行 Block 或 CallerRuns 策略。
     *  - 无返回值：操作不返回任何有用的数据给调用方。
     *  - 状态未知：任务被接受后，调用方无法直接获知任务是成功、失败还是仍在运行。
     * 
     * 因此，该任务不会直接在调用线程运行；全局队列已满时直接拒绝。
     */
    bool notifyWorker = false;
    const bool isWorkerThread = (m_currentPool == this && m_currentWorkerIndex != m_kNoWorker);
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_state != RuntimeState::Running) {
            m_metrics.OnRejected();
            throw TaskRejected("Cannot submit task: ThreadPool is not running");
        }

        if (isWorkerThread) {
            // 工作线程提交直接进入本地队列
            m_localQueues[m_currentWorkerIndex]->Push(std::move(task));
            m_pendingTasks.fetch_add(1, std::memory_order_release);

            m_metrics.OnSubmitted();
            m_metrics.OnLocalSubmitted();
            notifyWorker = true;
        } else {
            // 外部线程提交，全部进入全局队列；队列已满时保持非阻塞语义，直接拒绝。
            if (m_globalTasks.size() >= m_options.queueCapacity) {
                m_metrics.OnRejected();
                throw TaskRejected("Cannot submit fire-and-foget task: task queue is full");
            }

            m_globalTasks.push(std::move(task));
            m_pendingTasks.fetch_add(1, std::memory_order_release);
            m_metrics.OnSubmitted();
            notifyWorker = true;
        }
    }

    if (notifyWorker) {
        m_taskAvailableCv.notify_one();
    }
}

bool ThreadPool::TryPopLocal(std::size_t workerIndex, Task& task)
{
    if (!m_localQueues[workerIndex]->TryPop(task)) {
        return false;
    }

    m_pendingTasks.fetch_sub(1, std::memory_order_acq_rel);
    return true;
}

bool ThreadPool::TryPopGlobal(Task& task)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_globalTasks.empty()) {
            return false;
        }

        task = std::move(m_globalTasks.front());
        m_globalTasks.pop();
    }
    
    m_pendingTasks.fetch_sub(1, std::memory_order_acq_rel);
    m_queueNotFullCv.notify_one();
    return true;
}

bool ThreadPool::TrySteal(std::size_t workerIndex, Task& task)
{
    const std::size_t workerCount = m_localQueues.size();

    for (std::size_t offset = 1; offset < workerCount; ++offset) {
        const std::size_t victimIndex = (workerIndex + offset) % workerCount;
        if (m_localQueues[victimIndex]->TrySteal(task)) {
            m_pendingTasks.fetch_sub(1, std::memory_order_acq_rel);
            m_metrics.OnStolen();
            return true;
        }
    }

    return false;
}

bool ThreadPool::TryGetTask(std::size_t workerIndex, Task& task)
{
    if (m_discardPending.load(std::memory_order_acquire)) {
        return false;
    }

    // 1.优先执行自己的本地任务
    if (TryPopLocal(workerIndex, task)) {
        return true;
    }

    // 2.再处理外部提交的全局任务
    if (TryPopGlobal(task)) {
        return true;
    }

    // 3.最后尝试窃取其它 worker
    return TrySteal(workerIndex, task);
}

void ThreadPool::Shutdown()
{
    ShutdownImpl(ShutdownMode::Drain);
}

void ThreadPool::ShutdownNow()
{
    ShutdownImpl(ShutdownMode::DiscardPending);
}

RuntimeState ThreadPool::GetState() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

std::size_t ThreadPool::PendingTaskCount() const noexcept
{
    return m_pendingTasks.load(std::memory_order_acquire);
}

ThreadPoolOptions ThreadPool::GetOptions() const noexcept
{
    return m_options;
}

RuntimeMetricsSnapshot ThreadPool::GetMetrics() const noexcept
{
    return m_metrics.GetSnapshot();
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
     * 先在 m_mutex 保护下与 m_globalTasks 进行交换，再在锁外销毁，避免在持有任务队列锁时执行大量析构操作
     */
    std::vector<Task> discardedTasks;
    {
        std::lock_guard<std::mutex> taskLock(m_mutex);

        if (m_state == RuntimeState::Stopped) {
            return;
        }
        m_state = RuntimeState::Stopping;
        
        if (mode == ShutdownMode::DiscardPending) {
            while (!m_globalTasks.empty()) {
                discardedTasks.emplace_back(std::move(m_globalTasks.front()));
                m_globalTasks.pop();
            }
        }
    }

    if (mode == ShutdownMode::DiscardPending) {
        /*
         * m_state 已经变为 Stopping，后续 Submit 无法再向本地队列入队。
         *
         * 工作线程也会在下一轮检查 m_discardPending 后停止取新任务。
         */
        for (const auto& localQueue : m_localQueues) {
            localQueue->DrainTo(discardedTasks);
        }

        const std::size_t discardedCount = discardedTasks.size();
        if (discardedCount > 0) {
            m_pendingTasks.fetch_sub(discardedCount, std::memory_order_acq_rel);
            m_metrics.OnDiscarded(static_cast<std::uint64_t>(discardedCount));
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
     */
    discardedTasks.clear();

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

void ThreadPool::WorkerLoop(std::size_t workerIndex)
{
    m_currentPool = this;
    m_currentWorkerIndex = workerIndex;

    if (workerIndex >= m_localQueues.size()) {
        std::cerr << "[MiniRuntime] invalid worker index: " << workerIndex
            << ", queue count: " << m_localQueues.size() << '\n';

        std::terminate();
    }

    while (true) {
        if (m_discardPending.load(std::memory_order_acquire)) {
            break;
        }

        Task task;
        if (TryGetTask(workerIndex, task)) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                ++m_activeTasks;
            }

            try {
                task();
            } catch (const std::exception& e) {
                std::cerr << "[MiniRuntime] internal task exception: " << e.what() << '\n';
            } catch (...) {
                std::cerr << "[MiniRuntime] unknown internal task exception\n";
            }
            FinishTaskExecution();
            continue;
        }

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
            return m_state != RuntimeState::Running || m_pendingTasks.load(std::memory_order_acquire) > 0;
        });

        const bool shouldExit = 
            (m_state != RuntimeState::Running) &&
            (m_discardPending.load(std::memory_order_acquire) || m_pendingTasks.load(std::memory_order_acquire) == 0);
        if (shouldExit) {
            break;
        }
    }

    m_currentWorkerIndex = m_kNoWorker;
    m_currentPool = nullptr;
}

void ThreadPool::FinishTaskExecution() noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_activeTasks > 0) {
        m_activeTasks--;
    }

    if (m_activeTasks == 0 && m_pendingTasks.load(std::memory_order_acquire) == 0) {
        m_idleCv.notify_all();
    }
}

} // namespace minirt
