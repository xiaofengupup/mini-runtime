#include "minirt/thread_pool.h"
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <system_error>
#include <thread>
#include <vector>

namespace {

TEST(ThreadPoolLifecycleTest, RejectsZeroWorkerThreads)
{
    EXPECT_THROW(minirt::ThreadPool pool(0), std::invalid_argument);
}

TEST(ThreadPoolLifecycleTest, IsRunningAfterConstruction)
{
    minirt::ThreadPool pool(2);

    EXPECT_EQ(pool.GetState(), minirt::RuntimeState::Running);

    pool.Shutdown();
}

TEST(ThreadPoolLifecycleTest, ShutdownDrainsPendingTasks)
{
    minirt::ThreadPool pool(4);

    std::atomic<int> completed {0};
    std::vector<std::future<void>> futures;

    constexpr int taskCount = 1000;
    futures.reserve(taskCount);

    for (int i = 0; i < taskCount; ++i) {
        futures.push_back(
            pool.Submit([&completed] {
                completed.fetch_add(1, std::memory_order_relaxed);
            })
        );
    }

    pool.Shutdown();

    /*
     * Shutdown 返回时，所有排队任务都已经执行完毕。
     */
    EXPECT_EQ(completed.load(std::memory_order_relaxed), taskCount);

    for (std::future<void>& future : futures) {
        EXPECT_NO_THROW(future.get());
    }

    EXPECT_EQ(pool.GetState(), minirt::RuntimeState::Stopped);
}

TEST(ThreadPoolLifecycleTest, DestructorDrainsPendingTasks)
{
    constexpr int taskCount = 1000;
    std::atomic<int> completed{0};

    {
        minirt::ThreadPool pool(4);

        for (int i = 0; i < taskCount; ++i) {
            pool.Submit([&completed] {
                completed.fetch_add(1, std::memory_order_relaxed);
            });
        }
        // 不显式调用 Shutdown, 析构函数应执行优雅关闭。
    }

    EXPECT_EQ(completed.load(std::memory_order_relaxed), taskCount);
}

TEST(ThreadPoolLifecycleTest, ShutdownCanBeCalledRepeatedly)
{
    minirt::ThreadPool pool(2);

    EXPECT_NO_THROW(pool.Shutdown());
    EXPECT_NO_THROW(pool.Shutdown());
    EXPECT_NO_THROW(pool.Shutdown());

    EXPECT_EQ(pool.GetState(), minirt::RuntimeState::Stopped);
}

TEST(ThreadPoolLifecycleTest, ConcurrentShutdownIsSafe)
{
    minirt::ThreadPool pool(4);
    std::atomic<int> completed {0};

    constexpr int taskCount = 1000;
    for (int i = 0; i < taskCount; ++i) {
        pool.Submit([&completed] {
            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    std::vector<std::thread> shutdownThreads;
    constexpr int shutdownThreadCount = 4;
    shutdownThreads.reserve(shutdownThreadCount);

    for (int i = 0; i < shutdownThreadCount; ++i) {
        shutdownThreads.emplace_back([&pool] { pool.Shutdown(); });
    }

    for (std::thread& thread : shutdownThreads) {
        thread.join();
    }

    EXPECT_EQ(completed.load(std::memory_order_relaxed), taskCount);
    EXPECT_EQ(pool.GetState(), minirt::RuntimeState::Stopped);
}

TEST(ThreadPoolLifecycleTest, ShutdownNowDiscardsPendingTasks)
{
    /*
     * 只创建一个工作线程：
     * 第一个任务占住唯一工作线程，第二个任务只能留在队列里；
     * ShutdownNow 应当丢弃第二个任务。
     */
    minirt::ThreadPool pool(1);

    std::promise<void> taskStarted;
    std::future<void> taskStartedFuture = taskStarted.get_future();

    std::promise<void> allowTaskToFinish;
    std::shared_future<void> finishSignal = allowTaskToFinish.get_future().share();

    auto runningFuture = pool.Submit([&taskStarted, finishSignal] {
            taskStarted.set_value();
            finishSignal.wait(); // 保持第一个任务处于执行状态，确保第二个任务仍然停留在队列中。
            return 1;
        }
    );
    taskStartedFuture.wait();

    auto pendingFuture = pool.Submit([] { return 2; });

    /*
     * ShutdownNow 会等待正在运行的任务结束，因此不能直接在当前测试线程调用
     * 否则当前线程无法继续发送 allow_task_to_finish 信号。
     */
    std::thread shutdownThread([&pool] { pool.ShutdownNow(); });

    /*
     * 等待 ShutdownNow 把状态改为 Stopping。
     *
     * 使用截止时间，避免测试在实现错误时永久卡住。
     */
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (pool.GetState() == minirt::RuntimeState::Running && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }

    const bool observedStopping = (pool.GetState() == minirt::RuntimeState::Stopping);

    /*
     * 无论前面的观察是否成功，都必须释放运行任务，
     * 否则 shutdown_thread 无法结束。
     */
    allowTaskToFinish.set_value();
    shutdownThread.join();

    EXPECT_TRUE(observedStopping);
    EXPECT_EQ(runningFuture.get(), 1); // 已经开始执行的任务不会被强制取消。

    /*
     * 被丢弃的 packaged_task 没有执行，future 应得到 broken promise。
     */
    try {
        static_cast<void>(pendingFuture.get());
        FAIL() << "Pending task should have been discarded";
    } catch (const std::future_error& exception) {
        EXPECT_EQ(exception.code(), std::make_error_code(std::future_errc::broken_promise));
    }

    EXPECT_EQ(pool.GetState(), minirt::RuntimeState::Stopped);
}

}  // namespace