/**
 * 可取消异步任务的控制句柄
 */
#pragma once

#include "minirt/cancellation.h"

#include <chrono>
#include <future>
#include <memory>
#include <utility>

namespace minirt {

template <typename T>
class TaskHandle {
public:
    // 禁止拷贝
    TaskHandle(const TaskHandle&) = delete;
    TaskHandle& operator=(const TaskHandle&) = delete;

    // 支持移动
    TaskHandle(TaskHandle&&) noexcept = default;
    TaskHandle& operator=(TaskHandle&&) noexcept = default;

    /**
     * 请求取消任务，该操作不会强制终止工作线程
     */
    void Cancel() noexcept
    {
        if (m_state != nullptr) {
            m_state->requested.store(true, std::memory_order_relaxed);
        }
    }

    /**
     * 查询是否已经请求取消。
     */
    bool IsCancellationRequested() const noexcept
    {
        return m_state != nullptr && m_state->requested.load(std::memory_order_relaxed);
    }

    /**
     * 查询内部 future 是否仍然有效
     */
    bool Valid() const noexcept
    {
        return m_future.valid();
    }

    /**
     * 等待指定时间
     */
    template <typename Rep, typename Period>
    std::future_status WaitFor(const std::chrono::duration<Rep, Period>& timeout) const
    {
        return m_future.wait_for(timeout);
    }

    /**
     * 获取任务结果
     * 
     * 如果任务抛出异常，这里会直接抛出
     * 如果任务响应取消，会抛出 TaskCancelled
     */
    T Get()
    {
        return m_future.get();
    }

    /**
     * 在确实需要使用原始 future API 时访问 future
     */
    std::future<T>& GetFuture() noexcept
    {
        return m_future;
    }

private:
    friend class ThreadPool;

    TaskHandle(std::future<T>&& future, std::shared_ptr<detail::CancellationState> state)
        : future(std::move(future)), m_state(std::move(m_state)) {}

private:
    std::future<T> m_future; // std::future 本身是 move-only 类型，所以整个 TaskHandle 不支持拷贝
    std::shared_ptr<detail::CancellationState> m_state;
};

} // namespace minirt
