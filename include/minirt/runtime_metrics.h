/**
 * 线程池运行指标统计
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <iostream>

namespace minirt {

/**
 * 可复制的指标快照
 */
struct RuntimeMetricsSnapshot {
    std::uint64_t submitted {0};    // 被线程池接受的任务数量
    std::uint64_t completed {0};    // 正常完成的任务数量
    std::uint64_t failed {0};       // 以普通异常结束的任务数量
    std::uint64_t cancelled{0};     // 通过 TaskCancelled 结束的任务数量
    std::uint64_t rejected{0};      // 在提交阶段被拒绝的任务数量
    std::uint64_t discarded{0};     // ShutdownNow 丢弃的排队任务数量
    std::uint64_t callerRuns{0};    // 在提交线程中直接执行的任务数量

    std::uint64_t localSubmitted {0};   // 被提交到工作线程本地队列的任务数量
    std::uint64_t stolen {0};           // 被其它工作线程成功窃取的任务数量
};

/**
 * ThreadPool 内部使用的原子指标集合。
 */
class RuntimeMetrics {

public:
    RuntimeMetricsSnapshot GetSnapshot() const noexcept
    {
        RuntimeMetricsSnapshot snapshot;

        snapshot.submitted = m_submitted.load(std::memory_order_relaxed);
        snapshot.completed = m_completed.load(std::memory_order_relaxed);
        snapshot.failed = m_failed.load(std::memory_order_relaxed);
        snapshot.cancelled = m_cancelled.load(std::memory_order_relaxed);
        snapshot.rejected = m_rejected.load(std::memory_order_relaxed);
        snapshot.discarded = m_discarded.load(std::memory_order_relaxed);
        snapshot.callerRuns = m_callerRuns.load(std::memory_order_relaxed);
        snapshot.localSubmitted = m_localSubmitter.load(std::memory_order_relaxed);
        snapshot.stolen = m_stolen.load(std::memory_order_relaxed);

        return snapshot;
    }

private:
    friend class ThreadPool;

    void OnSubmitted() noexcept
    {
        m_submitted.fetch_add(1, std::memory_order_relaxed);
    }

    void OnCompleted() noexcept
    {
        m_completed.fetch_add(1, std::memory_order_relaxed);
    }

    void OnFailed() noexcept
    {
        m_failed.fetch_add(1, std::memory_order_relaxed);
    }

    void OnCancelled() noexcept
    {
        m_cancelled.fetch_add(1, std::memory_order_relaxed);
    }

    void OnRejected() noexcept
    {
        m_rejected.fetch_add(1, std::memory_order_relaxed);
    }

    void OnDiscarded(std::uint64_t count) noexcept
    {
        m_discarded.fetch_add(count, std::memory_order_relaxed);
    }

    void OnCallerRuns() noexcept
    {
        m_callerRuns.fetch_add(1, std::memory_order_relaxed);
    }

    void OnLocalSubmitted() noexcept
    {
        m_localSubmitter.fetch_add(1, std::memory_order_relaxed);
    }

    void OnStolen() noexcept
    {
        m_stolen.fetch_add(1, std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> m_submitted {0};
    std::atomic<std::uint64_t> m_completed {0};
    std::atomic<std::uint64_t> m_failed {0};
    std::atomic<std::uint64_t> m_cancelled {0};
    std::atomic<std::uint64_t> m_rejected {0};
    std::atomic<std::uint64_t> m_discarded {0};
    std::atomic<std::uint64_t> m_callerRuns {0};
    std::atomic<std::uint64_t> m_localSubmitter {0};
    std::atomic<std::uint64_t> m_stolen {0};
};

} // namespace minirt
