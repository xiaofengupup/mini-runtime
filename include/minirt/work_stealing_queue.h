/**
 * 工作窃取队列
 */
#pragma once

#include <mutex>
#include <deque>
#include <vector>
#include <utility>

namespace minirt {

template <typename T>
class WorkStealingQueue {
public:
    WorkStealingQueue() = default;

    WorkStealingQueue(const WorkStealingQueue&) = delete;
    WorkStealingQueue& operator=(const WorkStealingQueue&) = delete;
    WorkStealingQueue(WorkStealingQueue&&) = delete;
    WorkStealingQueue& operator=(WorkStealingQueue&&) = delete;

    /**
     * 本地线程提交任务
     */
    void Push(T item)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_queue.push_back(std::move(item));
    }

    /**
     * 所属工作线程从队尾取任务
     */
    bool TryPop(T& item)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_queue.empty()) {
            return false;
        }

        item = std::move(m_queue.back());
        m_queue.pop_back();
        return true;
    }

    /**
     * 其他 Worker 从队头窃取任务
     */
    bool TrySteal(T& item)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_queue.empty()) {
            return false;
        }

        item = std::move(m_queue.front());
        m_queue.pop_front();

        return true;
    }

    /**
     * 将所有剩余任务移动到外部容器。
     *
     * ShutdownNow 使用该接口清空本地队列。
     */
    std::size_t DrainTo(std::vector<T>& destination)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        const std::size_t count = m_queue.size();

        while (!m_queue.empty()) {
            destination.emplace_back(std::move(m_queue.front()));
            m_queue.pop_front();
        }
        
        return count;
    }

    std::size_t Size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size();
    }

    bool Empty() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.empty();
    }

private:
    mutable std::mutex m_mutex;
    std::deque<T> m_queue;
};
    
} // namespace minirt
