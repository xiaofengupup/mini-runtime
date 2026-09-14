#include "minirt/thread_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>

namespace {

using namespace std::chrono_literals;

TEST(PostTest, ExecutesTaskAndCountsCompletion)
{
    minirt::ThreadPool pool(2);

    std::atomic<bool> executed {false};
    pool.Post([&executed] {
        executed.store(true, std::memory_order_relaxed);
    });

    pool.Shutdown();

    EXPECT_TRUE(executed.load(std::memory_order_relaxed));

    const auto metrics = pool.GetMetrics();
    EXPECT_EQ(metrics.submitted, 1U);
    EXPECT_EQ(metrics.completed, 1U);
    EXPECT_EQ(metrics.failed, 0U);
}

TEST(PostTest, CountsFailureWithoutThrowingToCaller)
{
    minirt::ThreadPool pool(1);

    EXPECT_NO_THROW(pool.Post([] {
        throw std::runtime_error("expected post failure");
    }));

    pool.Shutdown();

    const auto metrics = pool.GetMetrics();
    EXPECT_EQ(metrics.submitted, 1U);
    EXPECT_EQ(metrics.completed, 0U);
    EXPECT_EQ(metrics.failed, 1U);
}

TEST(PostTest, SupportsMoveOnlyArguments)
{
    minirt::ThreadPool pool(1);

    std::atomic<int> result {0};
    pool.Post([](std::unique_ptr<int> value, std::atomic<int>& target) {
        target.store(*value, std::memory_order_relaxed);
    }, std::make_unique<int>(42), std::ref(result));

    pool.Shutdown();

    EXPECT_EQ(result.load(std::memory_order_relaxed), 42);
}

TEST(PostTest, RejectsSubmissionAfterShutdown)
{
    minirt::ThreadPool pool(1);

    pool.Shutdown();

    EXPECT_THROW(pool.Post([] {}), minirt::TaskRejected);
}

TEST(PostTest, RejectsImmediatelyWhenGlobalQueueIsFull)
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

    std::atomic<bool> pendingPostRan {false};
    pool.Post([&pendingPostRan] {
        pendingPostRan.store(true, std::memory_order_relaxed);
    });

    EXPECT_EQ(pool.PendingTaskCount(), 1U);

    auto rejectedPost = std::async(std::launch::async, [&pool] {
        try {
            pool.Post([] {});
            return false;
        } catch (const minirt::TaskRejected&) {
            return true;
        }
    });

    ASSERT_EQ(rejectedPost.wait_for(50ms), std::future_status::ready);
    EXPECT_TRUE(rejectedPost.get());

    release.set_value();
    runningFuture.get();
    pool.Shutdown();

    EXPECT_TRUE(pendingPostRan.load(std::memory_order_relaxed));

    const auto metrics = pool.GetMetrics();
    EXPECT_EQ(metrics.submitted, 2U);
    EXPECT_EQ(metrics.rejected, 1U);
}

} // namespace
