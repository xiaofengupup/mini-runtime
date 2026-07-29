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
            m_stopping = true;
        }

        m_cv.notify_all();

        for (std::thread& worker : m_workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        throw;
    }
}

ThreadPool::~ThreadPool()
{
    Stop();
}

void ThreadPool::Submit(Task task)
{
    if (task == nullptr) {
        throw std::invalid_argument("ThreadPool cannot submit an empty task");
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            throw std::runtime_error("Cannot submit task to a stopped ThreadPool");
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

void ThreadPool::Stop()
{
    std::call_once(m_stopOnce, [this]{
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
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
    });
}

void ThreadPool::WorkerLoop()
{
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] {
                return m_stopping || !m_tasks.empty();
            });

            /*
             * m_stopping 设置为 true 时，不能立即退出。
             * 
             * Shutdown 的语义是：已经提交到队列中的任务仍然执行完毕
             */
            if (m_stopping && m_tasks.empty()) {
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
         */
        try {
            task();
        } catch (const std::exception& e) {
            /*
             * Day 1 暂时没有 future，因此只能在工作线程中处理异常。
             * Day 2 会使用 packaged_task 和 future，将异常传递给调用 future.get() 的线程。
             */
            std::cerr << "[MiniRuntime] task threw an exception: " << e.what() << '\n';
        } catch (...) {
            std::cerr << "[MiniRuntime] task threw an unknown exception\n";
        }
    }
}

} // namespace minirt
