/**
 * 只支持移动的任务包装器
 * 
 * 背景：此前采用 std::function<void> + std::packaged_task 封装任务，但 std::function 要求内部对象可复制
 * 而 std::packaged_task 只支持移动，所以用 shared_ptr<packaged_task> 间接包装一层，带来了额外的堆分配和引用计数开销
 * 
 * 方案：MoveOnlyTask 本身支持移动调用，所以可以直接保存 move 捕获了 packaged_task 的 lambda。
 */
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace minirt {

class MoveOnlyTask {
public:
    MoveOnlyTask() noexcept = default;
    MoveOnlyTask(std::nullptr_t) noexcept {}

    /**
     * 核心方法：把任意可调用对象包装成统一的 MoveOnlyTask
     * 
     * @param callable 任意可调用对象，采用万能引用
     * 
     * DecayedCallable：退化拷贝，移除引用、顶层 const、数组/函数类型退化，使得 MoveOnlyTask 保存内部的 callable，不依赖调用者栈储的 callable
     * 
     * enable_if_t：SFINAE 约束，防止该模板构造函数误接管 MoveOnlyTask 自己的移动构造，否则编译器可能在 MoveOnlyTask other(std::move(task)) 时考虑这个模板，造成重载混乱。
     * 
     * std::is_invocable_r_v<void, DecayedCallable&>：要求 callable 能像 void() 一样被调用。返回值可以是 void，也可以是能被丢弃的普通返回值；但参数列表必须为空。这正好匹配线程池队列统一保存的任务形态：task()
     * 
     * std::make_unique<TaskCallableModel<DecayedCallable>>(...)：创建一个真正保存 callable 的派生对象，并把它放进 unique_ptr<TaskCallableBase>，实现类型擦除
     */
    template <typename Callable, typename DecayedCallable = std::decay_t<Callable>,
        typename = std::enable_if_t<!std::is_same_v<DecayedCallable, MoveOnlyTask> && std::is_invocable_r_v<void, DecayedCallable&>>>
    explicit MoveOnlyTask(Callable&& callable)
        : m_callable(std::make_unique<TaskCallableModel<DecayedCallable>>(std::forward<Callable>(callable)))
    {
    }

    MoveOnlyTask(const MoveOnlyTask&) = delete;
    MoveOnlyTask& operator=(const MoveOnlyTask&) = delete;

    MoveOnlyTask(MoveOnlyTask&&) noexcept = default;
    MoveOnlyTask& operator=(MoveOnlyTask&&) noexcept = default;

    /**
     * 支持 task() 调用
     */
    void operator()()
    {
        if (!m_callable) {
            throw std::bad_function_call();
        }

        m_callable->Invoke();
    }

    /**
     * 支持 if (task) { ... }
     */
    explicit operator bool() const noexcept
    {
        return static_cast<bool>(m_callable);
    }

    /**
     * 支持 task == nullptr 比较
     */
    bool operator==(std::nullptr_t) const noexcept
    {
        return !m_callable;
    }

    bool operator!=(std::nullptr_t) const noexcept
    {
        return static_cast<bool>(m_callable);
    }

private:
    struct TaskCallableBase {
        virtual ~TaskCallableBase() = default;
        virtual void Invoke() = 0;
    };

    template <typename Callable>
    struct TaskCallableModel final : TaskCallableBase {
        explicit TaskCallableModel(Callable&& callable) : m_callable(std::move(callable))
        {
        }

        explicit TaskCallableModel(const Callable& callable) : m_callable(callable)
        {
        }

        void Invoke() override
        {
            std::invoke(m_callable);
        }

        Callable m_callable;
    };

    std::unique_ptr<TaskCallableBase> m_callable;
};

} // namespace minirt
