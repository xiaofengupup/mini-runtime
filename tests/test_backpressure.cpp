#include "minirt/thread_pool.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <thread>

namespace {

using namespace std::chrono_literals;

TEST(BackpressureTest, RejectPolicyThrowsWhenQueueIsFull)
{
    minirt::ThreadPoolOptions options;
    options.threadCount = 1;
    options.queueCapacity = 1;
    options.rejectionPolicy = minirt::RejectionPolicy::Reject;

    minirt::ThreadPool pool(options);

    std::promise<void> started;
    std::future<void> startedFuture = started.get_future();

    std::promise<void> release;
    std::shared_future<void> releaseSignal = release.get_future().share();

    auto runningFuture = pool.Submit([&started, &releaseSignal] {
        started.set_value();
        releaseSignal.wait();
        return 1;
    });
    startedFuture.wait();

    // 唯一的工作线程正在执行 runningFuture，因此下面任务会占满容量为 1 的等待队列
    auto pendingFuture = pool.Submit([] { return 2; });
    
    EXPECT_EQ(pool.PendingTaskCount(), 1U);
    EXPECT_THROW(pool.Submit([] { return 3; }), minirt::TaskRejected);

    const auto metricsBeforeRelease = pool.GetMetrics();
    EXPECT_EQ(metricsBeforeRelease.submitted, 2U);
    EXPECT_EQ(metricsBeforeRelease.rejected, 1U);

    release.set_value();

    EXPECT_EQ(runningFuture.get(), 1);
    EXPECT_EQ(pendingFuture.get(), 2);

    pool.Shutdown();
}

TEST(BackpressureTest, CallerRunsExecutesTaskInSubmittingThread)
{
    minirt::ThreadPoolOptions options;
    options.threadCount = 1;
    options.queueCapacity = 1;
    options.rejectionPolicy = minirt::RejectionPolicy::CallerRuns;

    minirt::ThreadPool pool(options);

    std::promise<void> started;
    std::future<void> startedFuture = started.get_future();

    std::promise<void> release;
    std::shared_future<void> releaseSignal = release.get_future().share();

    auto runningFuture = pool.Submit([&started, &releaseSignal] {
        started.set_value();
        releaseSignal.wait();
    });
    startedFuture.wait();

    // 唯一的工作线程正在执行 runningFuture，因此下面任务会占满容量为 1 的等待队列
    auto pendingFuture = pool.Submit([] { return 2; });
    
    EXPECT_EQ(pool.PendingTaskCount(), 1U);

    const std::thread::id callThreadId = std::this_thread::get_id();

    auto callRunsFeature = pool.Submit([] { return std::this_thread::get_id(); });
    EXPECT_EQ(callRunsFeature.get(), callThreadId);

    const auto metrics = pool.GetMetrics();
    EXPECT_EQ(metrics.submitted, 3U);
    EXPECT_EQ(metrics.callerRuns, 1U);

    release.set_value();

    runningFuture.get();
    EXPECT_EQ(pendingFuture.get(), 2);

    pool.Shutdown();
}

TEST(BackpressureTest, BlockPolicyWaitsUntilQueueHasSpace)
{
    minirt::ThreadPoolOptions options;
    options.threadCount = 1;
    options.queueCapacity = 1;
    options.rejectionPolicy = minirt::RejectionPolicy::Block;

    minirt::ThreadPool pool(options);

    std::promise<void> started;
    auto startedFuture = started.get_future();

    std::promise<void> release;
    auto releaseSignal = release.get_future().share();

    auto runningFuture = pool.Submit([&started, releaseSignal] {
        started.set_value();
        releaseSignal.wait();
        return 1;
    });

    startedFuture.wait();

    auto pendingFuture = pool.Submit([] { return 2; });
    EXPECT_EQ(pool.PendingTaskCount(), 1U);

    // 返回类型为：future<future<int>>
    // std::launch::async：启动策略，必须在新的独立线程中立即异步执行任务
    auto blockedSubmission = std::async(std::launch::async, [&pool] {
        return pool.Submit([] { return 3; });
    });
    EXPECT_EQ(blockedSubmission.wait_for(50ms), std::future_status::timeout);

    release.set_value();

    std::future<int> thirdFuture = blockedSubmission.get();
    EXPECT_EQ(runningFuture.get(), 1);
    EXPECT_EQ(pendingFuture.get(), 2);
    EXPECT_EQ(thirdFuture.get(), 3);

    pool.Shutdown();
}

TEST(BackpressureTest, BlockedSubmitterIsReleasedWhenPoolShutsDown)
{
    minirt::ThreadPoolOptions options;
    options.threadCount = 1;
    options.queueCapacity = 1;
    options.rejectionPolicy = minirt::RejectionPolicy::Block;

    minirt::ThreadPool pool(options);

    std::promise<void> started;
    auto startedFuture = started.get_future();

    std::promise<void> release;
    auto releaseSignal = release.get_future().share();

    auto runningFuture = pool.Submit([&started, releaseSignal] {
        started.set_value();
        releaseSignal.wait();
    });

    startedFuture.wait();

    auto pendingFuture = pool.Submit([] { return 2; });

    auto blockedSubmitter = std::async(std::launch::async, [&pool] {
        try {
            auto future = pool.Submit([] { return 3; });
            
            static_cast<void>(future);
            return false;
        } catch (const minirt::TaskRejected&) {
            return true;
        }
    });
    EXPECT_EQ(blockedSubmitter.wait_for(50ms), std::future_status::timeout);

    auto shutdownFeature = std::async(std::launch::async, [&pool] {
        pool.ShutdownNow();
    });
    EXPECT_EQ(blockedSubmitter.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(blockedSubmitter.get());

    /*
     * ShutdownNow 仍在等待已经运行的任务，因此现在释放该任务。
     */
    release.set_value();

    shutdownFeature.get();
    runningFuture.get();

    EXPECT_THROW(pendingFuture.get(), std::future_error);
}

TEST(BackpressureTest, WorkerDoesNotDeadlockWhenBlockQueueIsFull)
{
    minirt::ThreadPoolOptions options;
    options.threadCount = 1;
    options.queueCapacity = 1;
    options.rejectionPolicy = minirt::RejectionPolicy::Block;

    minirt::ThreadPool pool(options);

    std::promise<void> outerStarted;
    auto outerStartedFuture = outerStarted.get_future();

    std::promise<void> allowNestedSubmit;
    auto allowNestedSignal = allowNestedSubmit.get_future().share();

    auto outerFuture = pool.Submit([&pool, &outerStarted, allowNestedSignal] {
        outerStarted.set_value();
        allowNestedSignal.wait();

        /*
         * 此时等待队列已满。
         *
         * 如果工作线程继续等待队列空间，会发生死锁。当前实现会退化为当前线程直接执行。
         */
        auto nested_future = pool.Submit([] { return 7; });
        return nested_future.get();
    });

    outerStartedFuture.wait();

    auto pendingFuture = pool.Submit([] { return 8; });
    EXPECT_EQ(pool.PendingTaskCount(), 1U);

    allowNestedSubmit.set_value();

    if (outerFuture.wait_for(1s) != std::future_status::ready) {
        pool.ShutdownNow();
        FAIL() << "Nested submission deadlocked";
        return;
    }

    EXPECT_EQ(outerFuture.get(), 7);
    EXPECT_EQ(pendingFuture.get(), 8);

    const auto metrics = pool.GetMetrics();
    EXPECT_EQ(metrics.callerRuns, 1U);

    pool.Shutdown();
}

} // namespace name
