/**
 * 基础线程池
 */
#pragma once

#include "minirt/cancellation.h"
#include "minirt/runtime_metrics.h"
#include "minirt/task_handle.h"
#include "minirt/thread_pool_options.h"
#include "minirt/work_stealing_queue.h"

#include <vector>
#include <thread>
#include <functional>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <tuple>
#include <future>
#include <memory>
#include <type_traits>
#include <utility>

namespace minirt {

/**
 * ThreadPool 生命周期状态
 */
enum class RuntimeState {
    Created,
    Running,
    Stopping,
    Stopped
};

class ThreadPool {
public:
    using Task = std::function<void()>;

    /**
     * 创建固定数量工作线程的线程池
     * 
     * @param threadCount 工作线程数量，必须大于 0
     */
    explicit ThreadPool(std::size_t threadCount);

    /**
     * 使用完整配置构造线程池
     */
    explicit ThreadPool(ThreadPoolOptions options);

    /**
     * 析构时停止线程池，并等待所有工作线程退出
     */
    ~ThreadPool();

    // 禁止拷贝和移动
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /**
     * 提交普通异步任务
     * 
     * 提交任意可调用对象；如果线程池已经停止，则抛出异常
     * 
     * 支持：
     * - 普通函数
     * - lambda
     * - 函数对象
     * - 成员函数指针
     * - 普通参数
     * - move-only 参数
     * - 返回值
     * - 异常传播
     * 
     * 代码含义：
     *   1. template<typename T, typename... Args>：变参模板（Variadic Template）
     *      F: 是要执行的函数或可调用对象，如 Lambda、函数指针、std::function 等。
     *      Args: 是传递给 F 的可变参数类型包（零个或多个参数）
     *   2. F&& function, Args&&... args：使用万能引用（Universal Reference），结合 std::forward 可以实现完美转发
     *   3. using FunctionType = std::decay_t<F>; 定义函数对象的真正存储类型；
     *   4. using ArgumentsTuple = std::tuple<std::decay_t<Args>...>; 定义一个元组（std::tuple）类型，用于将所有
     *      的实参打成一个包，以便存入任务队列或异步闭包中。
     * 
     * decay-copy：退化拷贝，理解这一概念是掌握异步任务的关键。
     *   问题场景：生命周期与安全引用。
     *   当调用 Submit 函数时，传入的参数可能是左值引用（例如 int&）、优质引用或者是带 const/volatile 修饰符的类型。
     *   如果直接使用原类型 F 和 Args... 去存储：
     *     - 如果用户传进来一个局部变量的引用（例如 int x = 10; Submit(func, x);），此时 Args 会被推导为 int&；
     *     - 异步任务的特点：Submit 函数会立即返回，而任务 Task 会在未来的某个时刻在另一个线程执行。
     *     - 如果任务内部保存的是 int&，当 Submit 调用结束时，原线程栈上的 x 可能已经被销毁（生命周期结束）。此时，异步线程
     *       再去访问这个引用，就会导致悬挂引用（Dangling Reference） 和 未定义行为（Undefined Behavior / 内存崩溃）。
     * 
     * 为什么需要使用 std::decay_t？
     *   std::decay_t<T> 的作用是模拟 C++ 按值传递时的类型转换规则（类似于数组退化为指针、函数退化为函数指针）。
     *     1. 移除引用：把 T& 和 T&& 变为 T。
     *     2. 移除 const 和 volatile 修饰符（针对顶层类型）。
     *     3. 数组/函数退化：将数组类型转换为指针，将函数类型转换为函数指针。
     *   使用 std::decay_t 可以确保存储在任务闭包/元组中的是具备独立生命周期的值对象，而不是依赖外部栈空间的引用对象。
     * 
     * 为什么需要 std::tuple？
     *   在 C ++ 中，Args.. 是一个参数包。参数包不能作为类的成员变量直接存储。要将多个类型、数量不定的参数作为一个整体
     *   存入任务对象（如 std::packaged_task 或自定义 Task 类）中，必须使用 std::tuple。
     *     - 打包：std::tuple<std::decay_t<Args>...> 可以将任意数量、任意类型的退化后参数“打成一个包裹”。
     *     - 解包执行：在异步线程执行任务时，可以配合 std::apply 轻松将 std::tuple 解包并传递给函数执行。
     * 
     * 万能引用：
     *  当 T&& 处于模板类型推导的环境下时，它不是普通的右值引用，而是万能引用：
     *    - 如果传进来的是左值，它被推导为左值引用；
     *    - 如果传进来的是右值，它被推导为右值引用或普通值；
     *  这保证了 Submit 接口可以接受任何类型、任何值类别（左值/右值）的函数和参数，然后，
     *  结合 std::make_tuple 和 std::forward 将参数完美转发并存入元组。
     * 
     */
    template<typename F, typename... Args>
    auto Submit(F&& function, Args&&... args)
    {
        /*
         * 异步任务不能依赖调用者栈上的转发引用
         * 因此需要将函数和参数 decay-copy 或 move 到任务对象内部
         */
        using FunctionType = std::decay_t<F>;
        using ArgumentsTuple = std::tuple<std::decay_t<Args>...>;

        /*
         * 推导异步函数 F 执行后的返回类型
         * 
         * std::invoke_result_t
         * 原型：std::invoke_result_t<Callable, ArgTypes...>
         * 是 C++ 17 引入的一个类型萃取（Type Trait）工具，用来替代 C++ 11 中被废弃的 std::result_of_t。
         * 其作用是：在不实际执行函数的情况下，问编译器：如果我把这组参数传递给这个可调用对象，它会返回什么类型。
         * 其中，写上 && 是为了精确推导当函数对象和参数以右值形式被调用时的返回值。
         */
        using ReturnType = std::invoke_result_t<FunctionType&&, std::decay_t<Args>&&...>;

        /*
         * std::packaged_task 是 C++ 11 引入的一个非常强大的异步任务打包工具。
         * std::packaged_task<Signature> 是一个模板类，它的模板参数 Signature 是一个函数签名（例如 int(int, double) 或者 void()）。
         * 它主要做两件事情：
         *   1. 打包装可调用对象：它可以把任何可调用对象（普通函数、lambda、std::bind、仿函数）打包起来。
         *   2. 连接 std::future：它内部关联了一个异步状态。当在某个线程中调用这个 packaged_task 时，
         *      它的执行结果（返回值或者抛出的异常）会自动写入这个异步状态中；而持有对应 std::future 的线程就可以获取这个结果。
         *
         * packaged_task 负责：
         * 1. 执行用户函数；
         * 2. 保存返回值；
         * 3. 捕获用户函数抛出的异常；
         * 4. 将结果或异常写入 future 的共享状态；
         * 
         * 这里创建一个无参形式的 packaged_task，签名是 ReturnType()
         * 由于 packaged_task 不支持拷贝（只能 move），为了能方便地放进 std::function 任务队列，
         * 通常用 std::make_shared 将其包裹在智能指针中。
         * 
         * 将任务包装成 void() 类型的可调用对象，放入任务队列
         * 
         * 主要是因为 std::packaged_task 是 move-only 类型。
         * Day 1 的任务队列保存 std::function<void()>，而 C++17 的 std::function 要求内部对象可复制。
         * shared_ptr 本身可以复制，因此捕获 shared_ptr 的 lambda 可以保存到 std::function 中。
         */

        auto userTask = [callable = FunctionType(std::forward<F>(function)),
                         arguments = ArgumentsTuple(std::forward<Args>(args)...)]() mutable -> ReturnType {
            return std::apply(std::move(callable), std::move(arguments));
        };

        auto taskAndFuture = MakeTask<ReturnType>(std::move(userTask));
        
        Dispatch(std::move(taskAndFuture.first));
        
        return std::move(taskAndFuture.second);
    }

    /**
     * 提交可取消任务
     * 
     * 用户函数的第一个参数必须能够接收 CancellationToken
     */
    template <typename F, typename... Args>
    auto SubmitCancelable(F&& function, Args&&... args)
    {
        using FunctionType = std::decay_t<F>;
        using ArgumentsTuple = std::tuple<std::decay_t<Args>...>;
        using ReturnType = std::invoke_result_t<FunctionType&&, CancellationToken, std::decay_t<Args>&&...>;

        auto cancellationState = std::make_shared<detail::CancellationState>();
        CancellationToken token(cancellationState);

        auto userTask = [
            callable = FunctionType(std::forward<F>(function)),
            arguments = ArgumentsTuple(std::forward<Args>(args)...),
            token
        ]() mutable -> ReturnType {
            /**
             * 任务开始前已经取消时，不调用用户函数
             */
            token.ThrowIfCancellationRequested();

            return std::apply([&callable, &token](auto&&... unpacked) mutable -> ReturnType {
                return std::invoke(std::move(callable), token, std::forward<decltype(unpacked)>(unpacked)...);
            }, std::move(arguments));
        };

        auto taskAndFuture = MakeTask<ReturnType>(std::move(userTask));

        Dispatch(std::move(taskAndFuture.first));

        return TaskHandle<ReturnType>(std::move(taskAndFuture.second), std::move(cancellationState));
    }

    /**
     * 优雅关闭
     * 
     * - 停止接受新任务
     * - 已排队任务继续执行
     * - 等待所有工作线程退出
     * - 可以重复调用
     */
    void Shutdown();

    /**
     * 尽快关闭
     * 
     * - 停止接收新任务；
     * - 丢弃尚未开始执行的任务；
     * - 已经开始运行的任务继续执行；
     * - 等待所有工作线程退出
     * - 可以重复调用；
     */
    void ShutdownNow();

    /**
     * 获取当前线程池状态
     */
    RuntimeState GetState() const;

     /**
     * 返回全局队列和所有工作线程本地队列中尚未开始执行的任务总数
     */
    std::size_t PendingTaskCount() const noexcept;

    ThreadPoolOptions GetOptions() const noexcept;

    RuntimeMetricsSnapshot GetMetrics() const noexcept;

private:
    enum class ShutdownMode {
        Drain,
        DiscardPending,
    };

    /**
     * 为用户任务增加：
     *
     * - packaged_task；
     * - future；
     * - 成功、失败和取消指标；
     * - std::function<void()> 类型擦除。
     */
    template <typename ReturnType, typename Callable>
    std::pair<Task, std::future<ReturnType>> MakeTask(Callable&& callable)
    {
        using CallableType = std::decay_t<Callable>;

        auto packagedTask = std::make_shared<std::packaged_task<ReturnType()>>(
            [this, userCallable = CallableType(std::forward<Callable>(callable))] () mutable -> ReturnType {
                try {
                    if constexpr (std::is_void_v<ReturnType>) {
                        std::invoke(std::move(userCallable));

                        m_metrics.OnCompleted();
                        return;
                    } else {
                        ReturnType result = std::invoke(std::move(userCallable));

                        m_metrics.OnCompleted();
                        return result;
                    }
                } catch (const TaskCancelled&) {
                    m_metrics.OnCancelled();
                    throw;
                } catch (...) {
                    m_metrics.OnFailed();
                    throw;
                }
            }
        );

        std::future<ReturnType> future = packagedTask->get_future();
        Task task = [packagedTask] {
            (*packagedTask)();
        };

        return std::make_pair(std::move(task), std::move(future));
    }

    /**
     * 根据队列状态和拒绝策略分发任务。
     */
    void Dispatch(Task task);

    bool TryGetTask(std::size_t workerIndex, Task& task);

    bool TryPopLocal(std::size_t workerIndex, Task& task);

    bool TryPopGlobal(Task& task);

    bool TrySteal(std::size_t workerIndex, Task& task);

    /**
     * Shutdown 和 ShutdownNow 的公共实现。
     */
    void ShutdownImpl(ShutdownMode mode);

    /**
     * 工作线程执行函数
     */
    void WorkerLoop(std::size_t workerIndex);

    /**
     * 当前正在运行的任务数量减一
     */
    void FinishTaskExecution() noexcept;

private:
    RuntimeState m_state { RuntimeState::Created };
    
    std::vector<std::thread> m_workers;
    
    // 外部线程提交的有界全局队列
    std::queue<Task> m_globalTasks; 

    // 每个工作线程拥有一个本地双端队列
    std::vector<std::unique_ptr<WorkStealingQueue<Task>>> m_localQueues; 

    ThreadPoolOptions m_options;

    // 已经离开任务队列、当前正在执行的任务数量，包含工作线程任务和 CallerRuns 任务
    std::size_t m_activeTasks {0}; 

    // 全局队列与所有本地队列中的待执行任务总数
    std::atomic<std::size_t> m_pendingTasks {0};

    // ShutdownNow 开始后，工作线程停止从队列获取新任务。
    std::atomic<bool> m_discardPending {false};

    /* 保护 m_tasks、m_state、m_activeTasks */
    mutable std::mutex m_mutex;

    /* 串行化多个并发的 Shutdown 调用 */
    std::mutex m_shutdownMutex;

    /**
     * 通知工作线程有任务可执行
     */
    std::condition_variable m_taskAvailableCv;

    /**
     * 通知 Block 策略下的提交线程队列出现空间
     */
    std::condition_variable m_queueNotFullCv;

    /**
     * 通知关闭线程所有活跃任务已经结束
     */
    std::condition_variable m_idleCv;
    
    /**
     * std::once_flag 是 C++ 11 引入的一个轻量级同步原语，位于 <mutex> 文件中。
     * 
     * 主要用途：确保某段代码在整个程序的生命周期中只被执行一次。通常与 std::call_once 函数配合使用。
     * 
     * 核心特性：
     *    1. 不可复制、不可移动；
     *    2. 非透明状态：无法通过 std::once_flag 的内部状态来判断它是否已经被执行过，只能通过 std::call_once 来触发
     *    3. 线程安全：在多线程环境下，std::call_once 能保证即使有多个线程同时尝试执行，也只有一个线程会真正执行目标函数，
     *                其它线程会被阻塞直到该函数执行完毕。
     * 
     * 此处用来保证 Stop() 中的停止和 join 逻辑只执行一次!
     */
    // std::once_flag m_stopOnce;

    /**
     * 线程池运行指标 
     */
    RuntimeMetrics m_metrics;

    /**
     * 标识当前线程是否正在执行本线程池任务
     * 
     * 用于：
     *  - 防止任务内部调用 Shutdown 导致自 join；
     *  - Block 策略下避免工作线程递归提交死锁。
     */
    static thread_local ThreadPool* m_currentPool;

    static thread_local std::size_t m_currentWorkerIndex;

    static constexpr std::size_t m_kNoWorker = std::numeric_limits<std::size_t>::max();
};

} // namespace minirt