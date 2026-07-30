/**
 * 取消机制
 */
#pragma once

#include <atomic>
#include <memory>
#include <stdexcept>
#include <utility>

namespace minirt {

namespace detail {

/**
 * TaskHandle 和 CancellationToken 共享的取消状态
 */
struct CancellationState {
    std::atomic<bool> requested { false };
};

} // namespace detail

/**
 * 任务观察到取消请求后抛出的异常
 */
class TaskCancelled : public std::runtime_error {
public:
    TaskCancelled() : std::runtime_error("task was cancelled") {}
};

/**
 * 传递给可取消任务的只读取消令牌
 */
class CancellationToken {
public:
    /**
     * 查询是否已经有人请求取消任务
     */
    bool IsCancellationRequested() const noexcept
    {
        return m_state != nullptr && m_state->requested.load(std::memory_order_relaxed);
    }

    /**
     * 如果请求已经取消，则抛出 TaskCancelled
     */
    void ThrowIfCancellationRequested() const
    {
        if (IsCancellationRequested()) {
            throw TaskCancelled();
        }
    }

private:
    friend class ThreadPool;
    explicit CancellationToken(std::shared_ptr<detail::CancellationState> state)
        : m_state(std::move(state)) {}

private:
    std::shared_ptr<detail::CancellationState> m_state;
};

} // namespace minirt
