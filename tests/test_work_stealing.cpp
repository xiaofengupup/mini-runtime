#include "minirt/thread_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

TEST(WorkStealingTest, InternalTasksCanBeStolenByAnotherWorker)
{
    minirt::ThreadPoolOptions options;
    options.threadCount = 2;
    options.queueCapacity = 16;
    options.rejectionPolicy = minirt::RejectionPolicy::Block;

    minirt::ThreadPool pool(options);

    constexpr std::size_t childCount = 100;

    auto outerFuture = pool.Submit([&pool] {
        const std::thread::id outerWorkerId = std::this_thread::get_id();
        std::vector<std::future<std::thread::id>> childFutures;
        childFutures.reserve(childCount);

        /*
         * 这些任务由工作线程内部提交，因此进入该工作线程的本地队列
         */
        for (std::size_t index = 0; index < childCount; ++index) {
            childFutures.push_back(
                pool.Submit([] { return std::this_thread::get_id(); })
            );
        }

        std::size_t executedByOther = 0;

        /*
         * 当前 Worker 等待子任务，另一个 Worker 必须窃取并执行它们
         */
        for (auto& future : childFutures) {
            if (future.get() != outerWorkerId) {
                ++executedByOther;
            }
        }
        return executedByOther;
    });

    ASSERT_EQ(outerFuture.wait_for(3s), std::future_status::ready) << "Child tasks were not stolen";
    EXPECT_GT(outerFuture.get(), 0U);

    pool.Shutdown();

    const auto metrics = pool.GetMetrics();
    EXPECT_EQ(metrics.localSubmitted, childCount);
    EXPECT_GT(metrics.stolen, 0U);
}

TEST(WorkStealingTest, MixedGlobalAndLocalTasksExecuteExactlyOnce)
{
    constexpr std::size_t outerCount = 20;
    constexpr std::size_t childrenPerOuter = 20;
    constexpr std::size_t childCount = outerCount * childrenPerOuter;

    minirt::ThreadPool pool(4);

    std::vector<std::atomic<int>> executionCounts(childCount);
    for (auto& count : executionCounts) {
        count.store(0, std::memory_order_relaxed);
    }

    std::vector<std::future<void>> outerFutures;
    outerFutures.reserve(outerCount);

    for (std::size_t outerIndex = 0; outerIndex < outerCount; ++outerIndex) {
        outerFutures.push_back(pool.Submit([&pool, &executionCounts, outerIndex] {
            for (std::size_t childIndex = 0; childIndex < childrenPerOuter; ++childIndex) {
                const std::size_t index = outerIndex * childrenPerOuter + childIndex;
                pool.Submit([&executionCounts, index] {
                    executionCounts[index].fetch_add(1,std::memory_order_relaxed);
                });
            }
        }));
    }

    /*
     * 确保所有子任务均已提交。
     */
    for (auto& future : outerFutures) {
        future.get();
    }

    /*
     * Shutdown 会排空：
     *
     * - 全局队列
     * - 所有本地队列
     */
    pool.Shutdown();

    for (std::size_t index = 0; index < childCount; ++index) {
        EXPECT_EQ(executionCounts[index].load(std::memory_order_relaxed), 1) << "child index = " << index;
    }

    const auto metrics = pool.GetMetrics();

    EXPECT_EQ(metrics.localSubmitted, childCount);
}

TEST(WorkStealingTest, SingleWorkerInternalSubmissionRunsInline)
{
    minirt::ThreadPool pool(1);

    auto outerFuture = pool.Submit([&pool]{
        auto childFuture = pool.Submit([] { return 42; });
        /*
         * 如果子任务进入本地队列，唯一 Worker 会在这里永久等待。
        */
        return childFuture.get();
    });

    ASSERT_EQ(outerFuture.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(outerFuture.get(), 42);

    pool.Shutdown();

    const auto metrics = pool.GetMetrics();
    EXPECT_EQ(metrics.callerRuns, 1U);
}

TEST(WorkStealingTest, ShutdownDrainsLocalQueues)
{
    constexpr std::size_t taskCount = 500;

    minirt::ThreadPool pool(4);

    std::atomic<std::size_t> completed {0};

    auto producerFuture = pool.Submit([&pool, &completed] {
        for (std::size_t index = 0; index < taskCount; ++index) {
            pool.Submit([&completed] {
                completed.fetch_add(1, std::memory_order_relaxed);
            });
        }
    });

    /*
     * producer 完成意味着所有子任务已经进入某个本地队列。
     */
    producerFuture.get();

    pool.Shutdown();

    EXPECT_EQ(completed.load(std::memory_order_relaxed), taskCount);

    EXPECT_EQ(pool.PendingTaskCount(), 0U);
}

}  // namespace