#include "minirt/thread_pool.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <thread>

namespace {

using namespace std::chrono_literals;

TEST(CancellationTest, CancelsTaskBeforeItStarts)
{
    minirt::ThreadPoolOptions options;
    options.threadCount = 1;
    options.queueCapacity = 4;

    minirt::ThreadPool pool(options);

    std::promise<void> blockerStarted;
    auto blockerStartedFuture = blockerStarted.get_future();

    std::promise<void> releaseBlocker;
    auto releaseSignal = releaseBlocker.get_future().share();

    auto blokerFuture = pool.Submit([&blockerStarted, releaseSignal] {
        blockerStarted.set_value();
        releaseSignal.wait();
    });

    blockerStartedFuture.wait();

    auto handle = pool.SubmitCancelable([](minirt::CancellationToken token) {
        token.ThrowIfCancellationRequested();
        return 42;
    });

    handle.Cancel();
    handle.Cancel();

    releaseBlocker.set_value();
    blokerFuture.get();

    EXPECT_THROW(handle.Get(), minirt::TaskCancelled);

    pool.Shutdown();

    const auto metrics = pool.GetMetrics();
    EXPECT_EQ(metrics.cancelled, 1U);
}

TEST( CancellationTest, RunningTaskCanCooperativelyObserveCancellation)
{
    minirt::ThreadPool pool(1);

    std::promise<void> taskStarted;
    auto taskStartedFuture = taskStarted.get_future();

    auto handle = pool.SubmitCancelable([&taskStarted](minirt::CancellationToken token) -> int {
        taskStarted.set_value();
        while (true) {
            token.ThrowIfCancellationRequested();
            std::this_thread::sleep_for(1ms);
        }
    });

    taskStartedFuture.wait();

    handle.Cancel();
    EXPECT_THROW(handle.Get(), minirt::TaskCancelled);

    pool.Shutdown();

    const auto metrics = pool.GetMetrics();
    EXPECT_EQ(metrics.cancelled, 1U);
    EXPECT_EQ(metrics.failed, 0U);
}

TEST(CancellationTest, CancellationDoesNotForceStopNonCooperativeTask)
{
    minirt::ThreadPool pool(1);

    std::promise<void> taskStarted;
    auto taskStartedFuture = taskStarted.get_future();

    std::promise<void> releaseTask;
    auto releaseSignal = releaseTask.get_future().share();

    auto handle = pool.SubmitCancelable([&taskStarted, releaseSignal](minirt::CancellationToken token) {
        taskStarted.set_value();
        static_cast<void>(token); // 故意不再检查 token。
        releaseSignal.wait();

        return 42;
    });

    taskStartedFuture.wait();

    handle.Cancel();
    releaseTask.set_value();

    /*
     * Cancel 只是请求取消，任务不主动检查 token，就仍然正常完成。
     */
    EXPECT_EQ(handle.Get(), 42);

    pool.Shutdown();

    const auto metrics = pool.GetMetrics();
    EXPECT_EQ(metrics.completed, 1U);
    EXPECT_EQ(metrics.cancelled, 0U);
}

TEST(CancellationTest, WaitForReportsTimeoutWithoutCancellingTask)
{
    minirt::ThreadPool pool(1);

    std::promise<void> releaseTask;
    auto releaseSignal = releaseTask.get_future().share();

    auto handle = pool.SubmitCancelable([releaseSignal](minirt::CancellationToken) {
        releaseSignal.wait();
        return 9;
    });

    EXPECT_EQ(handle.WaitFor(20ms), std::future_status::timeout);
    EXPECT_FALSE(handle.IsCancellationRequested());

    releaseTask.set_value();

    EXPECT_EQ(handle.Get(), 9);

    pool.Shutdown();
}

TEST(CancellationTest, SupportsVoidCancelableTask)
{
    minirt::ThreadPool pool(1);

    auto handle = pool.SubmitCancelable([](minirt::CancellationToken token) {
        token.ThrowIfCancellationRequested();
    });

    EXPECT_NO_THROW(handle.Get());
    pool.Shutdown();
}

}