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

/**
 * 多个生产者参与 Shutdown 并发时发生：
 * 
 * 1. Submit 成功返回的任务必须全部执行；
 * 2. Shutdown 开始后未成功提交的任务可以被拒绝；
 * 3. 不能丢失已经接收的任务
 */
TEST(StressTest, ConcurrentSubmitAndShutdownPreservesAcceptedTasks)
{
    minirt::ThreadPoolOptions options;
    options.threadCount = 4;
    options.queueCapacity = 64;
    options.rejectionPolicy = minirt::RejectionPolicy::Block;

    minirt::ThreadPool pool(options);

    constexpr std::size_t producerCount = 8;
    constexpr std::size_t taskPerProducer = 2000;
    std::atomic<std::size_t> accepted {0};
    std::atomic<std::size_t> completed {0};

    std::promise<void> start;
    std::shared_future<void> startSignal = start.get_future().share();

    std::vector<std::thread> producers;
    producers.reserve(producerCount);

    for (std::size_t idx = 0; idx < producerCount; ++idx) {
        producers.emplace_back([&pool, &accepted, &completed, startSignal] {
            startSignal.wait();

            for (std::size_t taskIdx = 0; taskIdx < taskPerProducer; ++taskIdx) {
                try {
                    auto future = pool.Submit([&completed]{
                        completed.fetch_add(1, std::memory_order_relaxed);
                    });

                    // 测试只关心任务是否被成功接受，不需要保留 future
                    static_cast<void>(future);
                    accepted.fetch_add(1, std::memory_order_relaxed);
                } catch (const minirt::TaskRejected&) {
                    break;
                }
            }
        });
    }

    start.set_value();

    // 让生产者与 Worker 运行一小段时间，然后触发关闭竞争
    std::this_thread::sleep_for(5ms);
    pool.Shutdown();

    for (std::thread& producer : producers) {
        producer.join();
    }

    EXPECT_EQ(completed.load(std::memory_order_relaxed), accepted.load(std::memory_order_relaxed));
    EXPECT_EQ(pool.PendingTaskCount(), 0U);
    EXPECT_EQ(pool.GetState(), minirt::RuntimeState::Stopped);
}

/**
 * 取消请求与任务执行存在竞争时，每个任务最终只能是正常完成或取消
 */
TEST(StressTest, CancellationResultsRemainConsistent)
{
    constexpr std::size_t taskCount = 1000;

    minirt::ThreadPoolOptions options;
    options.threadCount = 4;
    options.queueCapacity = taskCount;
    options.rejectionPolicy = minirt::RejectionPolicy::Block;

    minirt::ThreadPool pool(options);

    std::promise<void> beginExecution;
    std::shared_future<void> beginSignal = beginExecution.get_future().share();

    std::vector<minirt::TaskHandle<std::size_t>> handles;
    handles.reserve(taskCount);

    for (std::size_t index = 0; index < taskCount; ++index) {
        handles.push_back(pool.SubmitCancelable([beginSignal, index](minirt::CancellationToken token) {
            // 保证所有任务提交完、取消请求发出后，再开始检查取消状态
            beginSignal.wait();
            token.ThrowIfCancellationRequested();
            
            return index;
        }));
    }

    // 取消一半任务
    for (std::size_t idx = 0; idx < taskCount; idx += 2) {
        handles[idx].Cancel();
    }

    beginExecution.set_value();

    std::size_t completed = 0;
    std::size_t cancelled = 0;
    for (auto& handle : handles) {
        try {
            static_cast<void>(handle.Get());
            ++completed;
        } catch (const minirt::TaskCancelled&) {
            ++cancelled;
        }
    }

    pool.Shutdown();

    EXPECT_EQ(completed + cancelled, taskCount);
    EXPECT_EQ(cancelled, taskCount / 2);

    const auto metrics = pool.GetMetrics();
    EXPECT_EQ(metrics.submitted, taskCount);
    EXPECT_EQ(metrics.completed, completed);
    EXPECT_EQ(metrics.cancelled, cancelled);
    EXPECT_EQ(metrics.failed, 0U);
}

/**
 * 大量内部提交任务在本地队列和工作窃取之间迁移时，每个任务只能执行一次
 */
TEST(StressTest, NestedTasksExecuteExactlyOnce)
{
    constexpr std::size_t outerCount = 16;
    constexpr std::size_t childrenPerOuter = 200;
    constexpr std::size_t childCount = outerCount * childrenPerOuter;

    minirt::ThreadPool pool(4);

    std::vector<std::atomic<int>> executionCounts(childCount);
    for (auto& count : executionCounts) {
        count.store(0, std::memory_order_relaxed);
    }

    std::vector<std::future<void>> outerFutures;
    outerFutures.reserve(outerCount);

    for (std::size_t outerIdx = 0; outerIdx < outerCount; ++outerIdx) {
        outerFutures.push_back(pool.Submit([&pool, &executionCounts, outerIdx] {
            for (std::size_t childIdx = 0; childIdx < childrenPerOuter; ++childIdx) {
                const std::size_t taskIdx = outerIdx * childrenPerOuter + childIdx;
                pool.Submit([&executionCounts, taskIdx] {
                    executionCounts[taskIdx].fetch_add(1, std::memory_order_relaxed);
                });
            }
        }));
    }
    
    // 父任务完成，表示所有子任务都已经成功提交
    for (auto& future : outerFutures) {
        future.get();
    }

    pool.Shutdown();

    for (std::size_t idx = 0; idx < childCount; idx++) {
        EXPECT_EQ(executionCounts[idx].load(std::memory_order_relaxed), 1) << "task index = " << idx;
    }

    const auto metrics = pool.GetMetrics();
    EXPECT_EQ(metrics.localSubmitted, childCount);
    EXPECT_EQ(pool.PendingTaskCount(), 0U);
}

/**
 * 重复构造、启动和关闭线程池，检查最基础的线程生命周期是否稳定。
 */
TEST(StressTest, RepeatedConstructionAndShutdownIsSafe)
{
    constexpr std::size_t iterationCount = 2000;

    for (std::size_t iteration = 0; iteration < iterationCount; ++iteration) {
        minirt::ThreadPool pool(4);
        auto future = pool.Submit([iteration] {return iteration;});
        EXPECT_EQ(future.get(), iteration);

        pool.Shutdown();
        EXPECT_EQ(pool.GetState(), minirt::RuntimeState::Stopped);
    }
}

} // namespace
