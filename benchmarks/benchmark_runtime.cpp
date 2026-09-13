/**
 * 使用 std::chrono 实现可重复的轻量基准。
 */
#include "minirt/thread_pool.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct BenchmarkOptions {
    std::size_t threadCount {std::max(1U, std::thread::hardware_concurrency())};
    std::size_t taskCount {100000};
    std::size_t iterations {5};
    std::size_t warmup {1};

    bool help {false};
};

struct BenchmarkResult {
    std::string name;
    std::size_t threadCount {0};
    std::size_t taskCount {0};
    double submitSeconds {0.0};
    double waitSeconds {0.0};
    double shutdownSeconds {0.0};
    double totalSeconds {0.0};
    double nestedSubmitSeconds {0.0};
    double nestedWaitSeconds {0.0};
    std::uint64_t checksum {0};
    minirt::RuntimeMetricsSnapshot metrics {};
};

struct SummaryStats {
    double min {0.0};
    double median {0.0};
    double p95 {0.0};
};

void PrintUsage(const char* program)
{
    std::cout
        << "Usage:\n"
        << "  " << program << " [options]\n\n"
        << "Options:\n"
        << "  --threads N       Worker thread count.\n"
        << "  --tasks N         Number of tasks per measured iteration.\n"
        << "  --iterations N    Number of measured iterations. Default: 5.\n"
        << "  --warmup N        Number of warmup iterations. Default: 1.\n"
        << "  --help            Show this help.\n";
}

bool StartsWithDash(const std::string& value)
{
    return !value.empty() && value.front() == '-';
}

std::size_t ParseSize(const std::string& value, const char* name)
{
    if (value.empty() || StartsWithDash(value)) {
        throw std::invalid_argument(std::string(name) + " must be a non-negative integer");
    }

    std::size_t processed = 0;
    const unsigned long long parsed = std::stoull(value, &processed, 10);
    if (processed != value.size()) {
        throw std::invalid_argument(std::string(name) + " must be an integer");
    }

    if (parsed > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        throw std::out_of_range(std::string(name) + " is too large");
    }

    return static_cast<std::size_t>(parsed);
}

std::string RequireValue(int& index, int argc, char* argv[], const char* option)
{
    if (index + 1 >= argc) {
        throw std::invalid_argument(std::string(option) + " requires a value");
    }

    ++index;
    return argv[index];
}

BenchmarkOptions ParseOptions(int argc, char* argv[])
{
    BenchmarkOptions options;
    std::vector<std::string> positional;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];

        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--threads") {
            options.threadCount = ParseSize(RequireValue(index, argc, argv, "--threads"), "threads");
        } else if (argument == "--tasks") {
            options.taskCount = ParseSize(RequireValue(index, argc, argv, "--tasks"), "tasks");
        } else if (argument == "--iterations") {
            options.iterations = ParseSize(RequireValue(index, argc, argv, "--iterations"), "iterations");
        } else if (argument == "--warmup") {
            options.warmup = ParseSize(RequireValue(index, argc, argv, "--warmup"), "warmup");
        } else if (argument.rfind("--threads=", 0) == 0) {
            options.threadCount = ParseSize(argument.substr(10), "threads");
        } else if (argument.rfind("--tasks=", 0) == 0) {
            options.taskCount = ParseSize(argument.substr(8), "tasks");
        } else if (argument.rfind("--iterations=", 0) == 0) {
            options.iterations = ParseSize(argument.substr(13), "iterations");
        } else if (argument.rfind("--warmup=", 0) == 0) {
            options.warmup = ParseSize(argument.substr(9), "warmup");
        } else if (argument.rfind("--", 0) == 0) {
            throw std::invalid_argument("unknown option: " + argument);
        } else {
            positional.push_back(argument);
        }
    }

    if (!positional.empty()) {
        options.threadCount = ParseSize(positional[0], "threadCount");
    }
    if (positional.size() >= 2) {
        options.taskCount = ParseSize(positional[1], "taskCount");
    }
    if (positional.size() >= 3) {
        options.iterations = ParseSize(positional[2], "iterations");
    }
    if (positional.size() >= 4) {
        options.warmup = ParseSize(positional[3], "warmup");
    }
    if (positional.size() > 4) {
        throw std::invalid_argument("too many positional arguments");
    }

    if (options.threadCount == 0 || options.taskCount == 0 || options.iterations == 0) {
        throw std::invalid_argument("threadCount, taskCount and iterations must be greater than zero");
    }

    return options;
}

std::string FormatSeconds(double seconds)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(6) << seconds;
    return stream.str();
}

std::string FormatThroughput(double throughput)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(0) << throughput;
    return stream.str();
}

double MedianFromSorted(const std::vector<double>& values)
{
    if (values.empty()) {
        return 0.0;
    }

    const std::size_t middle = values.size() / 2;
    if (values.size() % 2 == 0) {
        return (values[middle - 1] + values[middle]) / 2.0;
    }

    return values[middle];
}

double NearestRankPercentileFromSorted(const std::vector<double>& values, double percentile)
{
    if (values.empty()) {
        return 0.0;
    }

    if (percentile <= 0.0) {
        return values.front();
    }
    if (percentile >= 1.0) {
        return values.back();
    }

    const double rank = std::ceil(percentile * static_cast<double>(values.size()));
    const std::size_t index = static_cast<std::size_t>(std::max(1.0, rank)) - 1;
    return values[std::min(index, values.size() - 1)];
}

std::vector<double> Collect(const std::vector<BenchmarkResult>& results, double BenchmarkResult::*field)
{
    std::vector<double> values;
    values.reserve(results.size());

    for (const auto& result : results) {
        values.push_back(result.*field);
    }

    return values;
}

SummaryStats Summarize(std::vector<double> values)
{
    if (values.empty()) {
        return {};
    }

    std::sort(values.begin(), values.end());

    return SummaryStats {
        values.front(),
        MedianFromSorted(values),
        NearestRankPercentileFromSorted(values, 0.95)
    };
}

void PrintTextSummary(const std::vector<BenchmarkResult>& results)
{
    if (results.empty()) {
        return;
    }

    const auto& last = results.back();
    const SummaryStats total = Summarize(Collect(results, &BenchmarkResult::totalSeconds));
    const SummaryStats submit = Summarize(Collect(results, &BenchmarkResult::submitSeconds));
    const SummaryStats wait = Summarize(Collect(results, &BenchmarkResult::waitSeconds));
    const SummaryStats shutdown = Summarize(Collect(results, &BenchmarkResult::shutdownSeconds));
    const SummaryStats nestedSubmit = Summarize(Collect(results, &BenchmarkResult::nestedSubmitSeconds));
    const SummaryStats nestedWait = Summarize(Collect(results, &BenchmarkResult::nestedWaitSeconds));
    const double throughput = total.median > 0.0
        ? static_cast<double>(last.taskCount) / total.median
        : 0.0;

    std::cout
        << std::left << std::setw(24) << last.name << "\n"
        << "threads=" << last.threadCount
        << " tasks=" << last.taskCount
        << " total[min/median/p95]="
        << FormatSeconds(total.min) << '/'
        << FormatSeconds(total.median) << '/'
        << FormatSeconds(total.p95) << "seconds"
        << " submit_med=" << FormatSeconds(submit.median) << "seconds"
        << " wait_med=" << FormatSeconds(wait.median) << "seconds"
        << " shutdown_med=" << FormatSeconds(shutdown.median) << "seconds";

    if (nestedSubmit.p95 > 0.0 || nestedWait.p95 > 0.0) {
        std::cout
            << " nested_submit_med=" << FormatSeconds(nestedSubmit.median) << "seconds"
            << " nested_wait_med=" << FormatSeconds(nestedWait.median) << "seconds";
    }

    std::cout
        << "\n throughput=" << FormatThroughput(throughput)
        << " submitted=" << last.metrics.submitted
        << " completed=" << last.metrics.completed
        << " local=" << last.metrics.localSubmitted
        << " stolen=" << last.metrics.stolen
        << " callerRuns=" << last.metrics.callerRuns
        << " checksum=" << last.checksum
        << "\n\n";
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

    const auto submitDone = Clock::now();

    std::uint64_t checksum = 0;
    for (auto& future : futures) {
        checksum += future.get();
    }

    const auto waitDone = Clock::now();

    pool.Shutdown();
    const auto shutdownDone = Clock::now();

    return BenchmarkResult {
        "external submission",
        threadCount,
        taskCount,
        std::chrono::duration<double>(submitDone - start).count(),
        std::chrono::duration<double>(waitDone - submitDone).count(),
        std::chrono::duration<double>(shutdownDone - waitDone).count(),
        std::chrono::duration<double>(shutdownDone - start).count(),
        0.0,
        0.0,
        checksum,
        pool.GetMetrics()
    };
}

BenchmarkResult RunNestedSubmission(std::size_t threadCount, std::size_t taskCount)
{
    minirt::ThreadPoolOptions options;
    options.threadCount = threadCount;
    options.queueCapacity = 4096;
    options.rejectionPolicy = minirt::RejectionPolicy::Block;

    minirt::ThreadPool pool(options);

    double childSubmitSeconds = 0.0;
    double childWaitSeconds = 0.0;

    const auto start = Clock::now();

    auto outerFuture = pool.Submit([&pool, taskCount, &childSubmitSeconds, &childWaitSeconds] {
        std::vector<std::future<std::uint64_t>> children;
        children.reserve(taskCount);

        const auto childSubmitStart = Clock::now();

        for (std::size_t index = 0; index < taskCount; ++index) {
            children.push_back(pool.Submit([index] {
                const std::uint64_t value = static_cast<std::uint64_t>(index);
                return value * 11400714819323198485ULL + (value >> 5U);
            }));
        }

        const auto childSubmitDone = Clock::now();

        std::uint64_t checksum = 0;
        for (auto& future : children) {
            checksum += future.get();
        }

        const auto childWaitDone = Clock::now();

        childSubmitSeconds = std::chrono::duration<double>(childSubmitDone - childSubmitStart).count();
        childWaitSeconds = std::chrono::duration<double>(childWaitDone - childSubmitDone).count();

        return checksum;
    });

    const auto submitDone = Clock::now();

    const std::uint64_t checksum = outerFuture.get();
    const auto waitDone = Clock::now();

    pool.Shutdown();
    const auto shutdownDone = Clock::now();

    return BenchmarkResult {
        "nested work stealing",
        threadCount,
        taskCount,
        std::chrono::duration<double>(submitDone - start).count(),
        std::chrono::duration<double>(waitDone - submitDone).count(),
        std::chrono::duration<double>(shutdownDone - waitDone).count(),
        std::chrono::duration<double>(shutdownDone - start).count(),
        childSubmitSeconds,
        childWaitSeconds,
        checksum,
        pool.GetMetrics()
    };
}

template <typename Runner>
std::vector<BenchmarkResult> RunIterations(
    Runner runner,
    std::size_t threadCount,
    std::size_t taskCount,
    std::size_t warmup,
    std::size_t iterations)
{
    for (std::size_t index = 0; index < warmup; ++index) {
        static_cast<void>(runner(threadCount, std::min<std::size_t>(taskCount, 1000)));
    }

    std::vector<BenchmarkResult> results;
    results.reserve(iterations);

    for (std::size_t index = 0; index < iterations; ++index) {
        results.push_back(runner(threadCount, taskCount));
    }

    return results;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const BenchmarkOptions options = ParseOptions(argc, argv);
        if (options.help) {
            PrintUsage(argv[0]);
            return 0;
        }

        std::cout << "[MiniRuntime benchmark] Please run in Release mode for meaningful results.\n"
                  << "[Options] threads=" << options.threadCount
                  << " tasks=" << options.taskCount
                  << " iterations=" << options.iterations
                  << " warmup=" << options.warmup
                  << "\n\n";

        const auto externalResults = RunIterations(
            RunExternalSubmission,
            options.threadCount,
            options.taskCount,
            options.warmup,
            options.iterations);

        const auto nestedResults = RunIterations(
            RunNestedSubmission,
            options.threadCount,
            options.taskCount,
            options.warmup,
            options.iterations);

        PrintTextSummary(externalResults);
        PrintTextSummary(nestedResults);
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "benchmark failed: " << exception.what() << '\n';
        std::cerr << "Use --help to show usage.\n";
        return 1;
    }
}
