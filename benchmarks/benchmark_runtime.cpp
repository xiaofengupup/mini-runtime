/**
 * 使用 std::chrono 实现可重复的轻量基准
 */
#include "minirt/thread_pool.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct BenchmarkResult {
    std::string name;
    std::size_t threadCount {0};
    std::size_t taskCount {0};
    double seconds {0.0};
    std::uint64_t checksum {0};
};

void PrintResult(const BenchmarkResult& result)
{
    const double throughput = result.seconds > 0.0 ? 
        static_cast<double>(result.taskCount) / result.seconds : 0.0;

    std::cout
        << std::left
        << std::setw(28)
        << result.name
        << " threads="
        << std::setw(3)
        << result.threadCount
        << " tasks="
        << std::setw(10)
        << result.taskCount
        << " time="
        << std::fixed
        << std::setprecision(4)
        << result.seconds
        << " s"
        << " throughput="
        << std::setprecision(0)
        << throughput
        << " tasks/s"
        << " checksum="
        << result.checksum
        << '\n';
}

BenchmarkResult RunExternalSubmission(std::size_t threadCount, std::size_t taskCount)
{
    minirt::ThreadPoolOptions options;
    options.threadCount = threadCount;
    options.queueCapacity = 4096;
    options.rejectionPolicy = minirt::RejectionPolicy::Block;

    minirt::ThreadPool pool(options);

    std::vector<std::future<std::uint64_t>> futures;
    futures.reserve(taskCount);

    const auto start = Clock::now();

    for (std::size_t index = 0; index < taskCount; ++index) {
        futures.push_back(pool.Submit([index] {
            const std::uint64_t value = static_cast<std::uint64_t>(index);
            return value * 2654435761ULL + (value >> 3U);
        }));
    }

    std::uint64_t checksum = 0;
    for (auto& future : futures) {
        checksum += future.get();
    }

    pool.Shutdown();

    const auto finish = Clock::now();
    const double seconds = std::chrono::duration<double>(finish - start).count();

    return BenchmarkResult {
        "external submission",
        threadCount,
        taskCount,
        seconds,
        checksum
    };
}

BenchmarkResult RunNestedSubmission(std::size_t threadCount, std::size_t taskCount)
{
    minirt::ThreadPoolOptions options;
    options.threadCount = threadCount;
    options.queueCapacity = 4096;
    options.rejectionPolicy = minirt::RejectionPolicy::Block;

    minirt::ThreadPool pool(options);

    const auto start = Clock::now();

    auto outerFuture = pool.Submit([&pool, taskCount] {
        std::vector<std::future<std::uint64_t>> children;
        children.reserve(taskCount);

        // 这些任务由 Worker 内部提交，因此进入本地队列并触发工作窃取。
        for (std::size_t index = 0; index < taskCount; ++index) {
            children.push_back(pool.Submit([index] {
                const std::uint64_t value = static_cast<std::uint64_t>(index);
                return value * 11400714819323198485ULL + (value >> 5U);
            }));
        }

        std::uint64_t checksum = 0;
        for (auto& future : children) {
            checksum += future.get();
        }
        return checksum;
    });

    const std::uint64_t checksum = outerFuture.get();

    pool.Shutdown();

    const auto finish = Clock::now();
    const double seconds = std::chrono::duration<double>(finish - start).count();

    return BenchmarkResult {
        "nested work stealing",
        threadCount,
        taskCount,
        seconds,
        checksum
    };
}

} // namspace

int main(int argc, char* argv[])
{
    try {
        std::size_t threadCount = std::max(1U, std::thread::hardware_concurrency());
        std::size_t taskCount = 100000;

        if (argc >= 2) {
            threadCount = static_cast<std::size_t>(std::stoull(argv[1]));
        }

        if (argc >= 3) {
            taskCount = static_cast<std::size_t>(std::stoull(argv[2]));
        }

        if (threadCount == 0 || taskCount == 0) {
            std::cerr << "threadCount and taskCount must be greater than zero\n";
            return 1;
        }

        std::cout << "MiniRuntime benchmark\n"
            << "Run in Release mode for meaningful results.\n\n";

        /*
         * 简单预热。
         */
        static_cast<void>(RunExternalSubmission(threadCount,1000));

        PrintResult(RunExternalSubmission(threadCount, taskCount));
        PrintResult(RunNestedSubmission(threadCount, taskCount));

        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "benchmark failed: " << exception.what() << '\n';
        return 1;
    }
}