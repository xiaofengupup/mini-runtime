#include "minirt/thread_pool.h"

#include <gtest/gtest.h>

#include <future>
#include <stdexcept>
#include <thread>

namespace {

TEST(RuntimeMetricsTest, CountsCompletedFailedAndCancelledTasks)
{
    minirt::ThreadPoolOptions options;
    options.threadCount = 1;
    options.queueCapacity = 8;

    minirt::ThreadPool pool(options);

    auto successfulFuture = pool.Submit([] { return 1; });
    auto failedFuture = pool.Submit([]() -> int {
        throw std::runtime_error("expected failure");
    });

    EXPECT_EQ(successfulFuture.get(), 1);

    EXPECT_THROW(failedFuture.get(), std::runtime_error);

    std::promise<void> blockerStarted;
    auto blockerStartedFuture = blockerStarted.get_future();

    std::promise<void> releaseBlocker;
    auto releaseSignal = releaseBlocker.get_future().share();

    auto blockerFuture = pool.Submit([&blockerStarted, releaseSignal] {
        blockerStarted.set_value();
        releaseSignal.wait();
    });
    blockerStartedFuture.wait();

    auto cancelledHandle = pool.SubmitCancelable([](minirt::CancellationToken token) {
        token.ThrowIfCancellationRequested();
        return 4;
    });

    cancelledHandle.Cancel();
    releaseBlocker.set_value();
    blockerFuture.get();

    EXPECT_THROW(cancelledHandle.Get(), minirt::TaskCancelled);

    pool.Shutdown();

    const auto metrics = pool.GetMetrics();

    EXPECT_EQ(metrics.submitted, 4U);
    EXPECT_EQ(metrics.completed, 2U);
    EXPECT_EQ(metrics.failed, 1U);
    EXPECT_EQ(metrics.cancelled, 1U);
    EXPECT_EQ(metrics.rejected, 0U);
}

TEST(RuntimeMetricsTest, CountsTasksDiscardedByShutdownNow)
{
    minirt::ThreadPool pool(1);

    std::promise<void> taskStarted;
    auto taskStartedFuture = taskStarted.get_future();

    std::promise<void> releaseTask;
    auto releaseSignal = releaseTask.get_future().share();

    auto running_future = pool.Submit([&taskStarted, releaseSignal] {
        taskStarted.set_value();
        releaseSignal.wait();
        return 1;
    });

    taskStartedFuture.wait();

    auto pendingOne = pool.Submit([] { return 2; });
    auto pendingTwo = pool.Submit([] { return 3; });
    auto shutdownFuture = std::async( std::launch::async, [&pool] {
        pool.ShutdownNow();
    });

    while (pool.GetState() == minirt::RuntimeState::Running) {
        std::this_thread::yield();
    }

    releaseTask.set_value();

    shutdownFuture.get();

    EXPECT_EQ(running_future.get(), 1);
    EXPECT_THROW(pendingOne.get(), std::future_error);
    EXPECT_THROW(pendingTwo.get(), std::future_error);

    const auto metrics = pool.GetMetrics();
    EXPECT_EQ(metrics.submitted, 3U);
    EXPECT_EQ(metrics.completed, 1U);
    EXPECT_EQ(metrics.discarded, 2U);
}

}  // namespace