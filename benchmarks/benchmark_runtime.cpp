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
#include <atomic>

namespace {

using Clock = std::chrono::steady_clock;

enum class TaskType {
    Empty,          // 空任务
    LightCompute,   // 轻计算
    MediumCompute,  // 中计算
    HeavyCompute,   // 重计算
    End
};

const char* TaskTypeName(TaskType type)
{
    switch (type) {
        case TaskType::Empty:
            return "empty";
        case TaskType::LightCompute:
            return "light";
        case TaskType::MediumCompute:
            return "medium";
        case TaskType::HeavyCompute:
            return "heavy";
        default:
            return "unknown";
    }
}

struct TaskRange {
    std::size_t begin;
    std::size_t end;
};

TaskRange RangeForProducer(std::size_t taskCount, std::size_t producerIndex, std::size_t producerCount)
{
    const std::size_t base = taskCount / producerCount;
    const std::size_t remainder = taskCount % producerCount;

    const std::size_t begin = producerIndex * base + std::min(producerIndex, remainder);
    const std::size_t count = base + (producerIndex < remainder ? 1 : 0);

    return TaskRange {begin, begin + count};
}

struct BenchmarkOptions {
    std::size_t threadCount {std::max(1U, std::thread::hardware_concurrency())};
    std::size_t taskCount {100000};
    std::size_t producerCount {2};
    std::size_t iterations {5};
    std::size_t warmup {1};
    TaskType taskType {TaskType::LightCompute};  // 默认任务类型为轻计算

    bool help {false};
};

struct BenchmarkResult {
    std::string name;
    std::size_t threadCount {0};
    std::size_t taskCount {0};
    std::size_t producerCount {1};
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
        << "  --producers N     External producer thread count. Default: 2.\n"
        << "  --task-type VALUE Task type: 0/empty, 1/light, 2/medium, 3/heavy. Default: light.\n"
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

bool IsUnsignedInteger(const std::string& value)
{
    if (value.empty()) {
        return false;
    }

    return std::all_of(value.begin(), value.end(), [](char ch) {
        return ch >= '0' && ch <= '9';
    });
}

TaskType ParseTaskType(const std::string& value)
{
    if (IsUnsignedInteger(value)) {
        const std::size_t parsed = ParseSize(value, "task-type");
        const std::size_t max = static_cast<std::size_t>(TaskType::End);
        if (parsed >= max) {
            throw std::invalid_argument("task-type must be less than " + std::to_string(max));
        }

        return static_cast<TaskType>(parsed);
    }

    if (value == "empty") {
        return TaskType::Empty;
    }
    if (value == "light") {
        return TaskType::LightCompute;
    }
    if (value == "medium") {
        return TaskType::MediumCompute;
    }
    if (value == "heavy") {
        return TaskType::HeavyCompute;
    }

    throw std::invalid_argument("task-type must be one of: 0, empty, 1, light, 2, medium, 3, heavy");
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

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];

        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--threads") {
            options.threadCount = ParseSize(RequireValue(index, argc, argv, "--threads"), "threads");
        } else if (argument == "--tasks") {
            options.taskCount = ParseSize(RequireValue(index, argc, argv, "--tasks"), "tasks");
        } else if (argument == "--producers") {
            options.producerCount = ParseSize(RequireValue(index, argc, argv, "--producers"), "producers");
        }  else if (argument == "--task-type") {
            options.taskType = ParseTaskType(RequireValue(index, argc, argv, "--task-type"));
        } else if (argument == "--iterations") {
            options.iterations = ParseSize(RequireValue(index, argc, argv, "--iterations"), "iterations");
        } else if (argument == "--warmup") {
            options.warmup = ParseSize(RequireValue(index, argc, argv, "--warmup"), "warmup");
        } else if (argument.rfind("--threads=", 0) == 0) {
            options.threadCount = ParseSize(argument.substr(10), "threads");
        } else if (argument.rfind("--tasks=", 0) == 0) {
            options.taskCount = ParseSize(argument.substr(8), "tasks");
        } else if (argument.rfind("--producers=", 0) == 0) {
            options.producerCount = ParseSize(argument.substr(12), "producers");
        } else if (argument.rfind("--task-type=", 0) == 0) {
            options.taskType = ParseTaskType(argument.substr(12));
        } else if (argument.rfind("--iterations=", 0) == 0) {
            options.iterations = ParseSize(argument.substr(13), "iterations");
        } else if (argument.rfind("--warmup=", 0) == 0) {
            options.warmup = ParseSize(argument.substr(9), "warmup");
        } else if (argument.rfind("--", 0) == 0) {
            throw std::invalid_argument("unknown option: " + argument);
        } else {
            throw std::invalid_argument("too many arguments");
        }
    }

    if (options.threadCount == 0 || options.taskCount == 0 || options.iterations == 0 || options.producerCount == 0) {
        throw std::invalid_argument("threadCount, taskCount, producers and iterations must be greater than zero");
    }

    return options;
}

std::string FormatMilliseconds(double seconds)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3) << seconds * 1000.0;
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
        << ", tasks=" << last.taskCount
        << ", producers=" << last.producerCount
        << ", total_ms[min/median/p95]="
        << FormatMilliseconds(total.min) << '/'
        << FormatMilliseconds(total.median) << '/'
        << FormatMilliseconds(total.p95) << "ms"
        << ", submit_med_ms=" << FormatMilliseconds(submit.median) << "ms"
        << ", wait_med_ms=" << FormatMilliseconds(wait.median) << "ms"
        << ", shutdown_med_ms=" << FormatMilliseconds(shutdown.median) << "ms";

    if (nestedSubmit.p95 > 0.0 || nestedWait.p95 > 0.0) {
        std::cout
            << ", nested_submit_med_ms=" << FormatMilliseconds(nestedSubmit.median) << "ms"
            << ", nested_wait_med_ms=" << FormatMilliseconds(nestedWait.median) << "ms";
    }

    std::cout
        << "\n throughput=" << FormatThroughput(throughput)
        << ", submitted=" << last.metrics.submitted
        << ", completed=" << last.metrics.completed
        << ", local=" << last.metrics.localSubmitted
        << ", stolen=" << last.metrics.stolen
        << ", callerRuns=" << last.metrics.callerRuns
        << ", checksum=" << last.checksum
        << "\n\n";
}

/**
 * Mix主要处理是打散比特，制造真实一点的整数计算负载
 *
 * value ^= value >> 33U：把高位信息折叠到低位，让 bit 之间互相影响。
 * 乘以大常数：在 uint64_t 的模运算空间里快速扩散 bit。输入某一位变化，经过乘法后会影响很多输出位。
 * 多轮重复：让结果看起来更像 hash mixing，避免简单的 value += index 这类计算过于容易被 CPU 或编译器简化。
 *
 * 这些常数本质上不是魔数，是哈希函数 finalizer 里常见的 bit-mixing 常数。
 */
std::uint64_t Mix(std::uint64_t value)
{
    value ^= value >> 33U;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33U;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33U;

    return value;
}

std::uint64_t RunSyntheticWork(TaskType type, std::size_t index)
{
    std::uint64_t value = static_cast<std::uint64_t>(index) + 0x9e3779b97f4a7c15ULL;

    std::size_t rounds = 0;
    switch (type) {
        case TaskType::Empty:
            return value;
        case TaskType::LightCompute:
            rounds = 32;
            break;
        case TaskType::MediumCompute:
            rounds = 1024;
            break;
        case TaskType::HeavyCompute:
            rounds = 32768;
            break;
        default:
            throw std::logic_error("unknown task type");
    }

    for (std::size_t round = 0; round < rounds; ++round) {
        value = Mix(value + static_cast<std::uint64_t>(round));
    }

    return value;
}

std::string ScenarioName(const char* base, TaskType taskType)
{
    return std::string(base) + "/" + TaskTypeName(taskType);
}

BenchmarkResult RunExternalSubmission(std::size_t threadCount, std::size_t taskCount, TaskType taskType)
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
        futures.push_back(pool.Submit([taskType, index] {
            return RunSyntheticWork(taskType, index);
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
        ScenarioName("external submission", taskType),
        threadCount,
        taskCount,
        1,
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

BenchmarkResult RunNestedSubmission(std::size_t threadCount, std::size_t taskCount, TaskType taskType)
{
    minirt::ThreadPoolOptions options;
    options.threadCount = threadCount;
    options.queueCapacity = 4096;
    options.rejectionPolicy = minirt::RejectionPolicy::Block;

    minirt::ThreadPool pool(options);

    double childSubmitSeconds = 0.0;
    double childWaitSeconds = 0.0;

    const auto start = Clock::now();

    auto outerFuture = pool.Submit([&pool, taskCount, taskType, &childSubmitSeconds, &childWaitSeconds] {
        std::vector<std::future<std::uint64_t>> children;
        children.reserve(taskCount);

        const auto childSubmitStart = Clock::now();

        for (std::size_t index = 0; index < taskCount; ++index) {
            children.push_back(pool.Submit([taskType, index] {
                return RunSyntheticWork(taskType, index);
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
        ScenarioName("nested work stealing", taskType),
        threadCount,
        taskCount,
        1,
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

BenchmarkResult RunMultiProducerExternalSubmission(
    std::size_t threadCount, std::size_t taskCount, TaskType taskType, std::size_t producerCount)
{

    minirt::ThreadPoolOptions options;
    options.threadCount = threadCount;
    options.queueCapacity = 4096;
    options.rejectionPolicy = minirt::RejectionPolicy::Block;

    minirt::ThreadPool pool(options);

    std::vector<std::vector<std::future<std::uint64_t>>> futuresByProducer(producerCount);
    std::vector<std::thread> producerThreads;
    std::vector<std::exception_ptr> errors(producerCount);

    producerThreads.reserve(producerCount);

    std::promise<void> startPromise;
    std::shared_future<void> startSignal = startPromise.get_future().share();
    std::atomic<std::size_t> readyCount {0};

    for (std::size_t producerIndex = 0; producerIndex < producerCount; ++producerIndex) {
        producerThreads.emplace_back([&, producerIndex] {
            readyCount.fetch_add(1, std::memory_order_release);
            startSignal.wait();

            try {
                const TaskRange range = RangeForProducer(taskCount, producerIndex, producerCount);
                auto& futures = futuresByProducer[producerIndex];
                futures.reserve(range.end - range.begin);

                for (std::size_t index = range.begin; index < range.end; ++index) {
                    futures.push_back(pool.Submit([taskType, index] {
                        return RunSyntheticWork(taskType, index);
                    }));
                }
            } catch (...) {
                errors[producerIndex] = std::current_exception();
            }
        });
    }

    while (readyCount.load(std::memory_order_acquire) < producerCount) {
        std::this_thread::yield();
    }

    const auto start = Clock::now();
    startPromise.set_value();

    for (auto& producerThread : producerThreads) {
        producerThread.join();
    }

    const auto submitDone = Clock::now();

    for (const auto& error : errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }

    std::uint64_t checksum = 0;
    for (auto& futures : futuresByProducer) {
        for (auto& future : futures) {
            checksum += future.get();
        }
    }

    const auto waitDone = Clock::now();

    pool.Shutdown();
    const auto shutdownDone = Clock::now();

    return BenchmarkResult {
        ScenarioName("multi producer external", taskType),
        threadCount,
        taskCount,
        producerCount,
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

template <typename Runner>
std::vector<BenchmarkResult> RunIterations(
    Runner runner,
    std::size_t threadCount,
    std::size_t taskCount,
    TaskType taskType,
    std::size_t warmup,
    std::size_t iterations)
{
    for (std::size_t index = 0; index < warmup; ++index) {
        static_cast<void>(runner(threadCount, std::min<std::size_t>(taskCount, 1000), taskType));
    }

    std::vector<BenchmarkResult> results;
    results.reserve(iterations);

    for (std::size_t index = 0; index < iterations; ++index) {
        results.push_back(runner(threadCount, taskCount, taskType));
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
                  << " producers=" << options.producerCount
                  << " task-type=" << TaskTypeName(options.taskType)
                  << " iterations=" << options.iterations
                  << " warmup=" << options.warmup
                  << "\n\n";

        // 单生产者外部提交
        const auto externalResults = RunIterations(
            RunExternalSubmission,
            options.threadCount, options.taskCount, options.taskType, options.warmup, options.iterations);

        // 单生产者嵌套提交
        const auto nestedResults = RunIterations(
            RunNestedSubmission,
            options.threadCount, options.taskCount, options.taskType, options.warmup, options.iterations);

        // 多生产者外部提交
        const auto multiProducersResults = RunIterations(
            [producerCount = options.producerCount](std::size_t threadCount, std::size_t taskCount, TaskType taskType) {
                return RunMultiProducerExternalSubmission(threadCount, taskCount, taskType, producerCount);
            },
            options.threadCount, options.taskCount, options.taskType,
            options.warmup, options.iterations
        );

        PrintTextSummary(externalResults);
        PrintTextSummary(nestedResults);
        PrintTextSummary(multiProducersResults);
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "benchmark failed: " << exception.what() << '\n';
        std::cerr << "Use --help to show usage.\n";
        return 1;
    }
}
