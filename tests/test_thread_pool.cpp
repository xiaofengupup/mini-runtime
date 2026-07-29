#include "minirt/thread_pool.h"

#include <atomic>
#include <stdexcept>

namespace {

/**
 * EXPECT：失败后测试仍继续运行
 * ASSERT：失败后立即终止当前测试函数
 */

TEST(ThreadPoolTest, ExecutesAllSubmittedTasks)
{
    minirt::ThreadPool pool(4);

    constexpr int taskCount = 1000;
    std::atomic<int> counter {0};

    for (int i = 0; i < taskCount; ++i) {
        pool.Submit([&count] {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }

    pool.Stop();

    EXPECT_EQ(counter.load(std::memory_order_relaxed), taskCount);
}

TEST(ThreadPoolTest, SupportsMultipleProducerThreads)
{
    minirt::ThreadPool pool(4);

    constexpr int producerCount = 4;
    constexpr int taskPerProducer = 250;

    std::atomic<int> counter {0};
    std::vector<std::thread> producers;
    producers.reserve(producerCount);

    for (int i = 0; i < producerCount; ++i) {
        producers.emplace_back([&pool, &counter] {
            for (int j = 0; j < taskPerProducer; ++j) {
                pool.Submit([&counter] {
                    counter.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
    }

    for (std::thread& producer : producers) {
        producer.join();
    }

    pool.Stop();

    EXPECT_EQ(counter.load(std::memory_order_relaxed), producerCount * taskPerProducer);
}

TEST(ThreadPoolTest, DestructorExecutesRemainingTasks)
{
    constexpr int taskCount = 1000;
    std::atomic<int> counter {0};

    {
        minirt::ThreadPool pool(4);
        for (int i = 0; i < taskCount; ++i) {
            pool.Submit([&counter] {
                counter.fetch_add(1, std::memory_order_relaxed);
            });
        }

        // 不显式调用 Stop()，析构函数应等待已经提交的任务执行完成。
    }

    EXPECT_EQ(counter.load(std::memory_order_relaxed), taskCount);
}

TEST(ThreadPoolTest, CanStopWithoutAnyTasks)
{
    minirt::ThreadPool pool(4);
    EXPECT_NO_THROW(pool.Stop());
}

TEST(ThreadPoolTest, StopCanBeCalledRepeatedly)
{
    minirt::ThreadPool pool(4);

    EXPECT_NO_THROW(pool.Stop());
    EXPECT_NO_THROW(pool.Stop());
    EXPECT_NO_THROW(pool.Stop());
}

TEST(ThreadPoolTest, RejectsTaskSubmittedAfterStop)
{
    minirt::ThreadPool pool(2);
    pool.Stop();

    EXPECT_THROW(pool.Submit([] {}), std::runtime_error);
}

TEST(ThreadPoolTest, RejectsEmptyTask)
{
    minirt::ThreadPool pool(2);
    minirt::ThreadPool::Task emptyTask;

    EXPECT_THROW(pool.Submit(std::move(empty_task)), std::invalid_argument);
    pool.Stop();
}

TEST(ThreadPoolTest, RejectsZeroWorkerThreads)
{
    EXPECT_THROW(minirt::ThreadPool pool(0), std::invalid_argument);
}

TEST(ThreadPoolTest, WorkerSurvivesTaskException)
{
    minirt::ThreadPool pool(2);
    std::atomic<int> completed {0};

    pool.Submit([] {
        throw std::runtime_error("expected task failure");
    });

    pool.Submit([&completed] {
        completed.fetch_add(1, std::memory_order_relaxed);
    });

    pool.Stop();

    // 前一个任务抛出异常后，工作线程不应导致进程终止。
    EXPECT_EQ(completed.load(std::memory_order_relaxed), 1);
}

}