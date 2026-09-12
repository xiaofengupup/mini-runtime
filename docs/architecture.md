# MiniRuntime 架构

本文基于当前源码描述 MiniRuntime 的实现结构、任务生命周期和并发不变量。

## 设计目标

MiniRuntime 关注一个轻量级 C++ 异步运行时的核心问题：

- 用统一接口提交不同类型的可调用对象。
- 在线程之间传递返回值和异常。
- 用有界队列表达背压，避免无限积压。
- 支持协作式取消，而不是强制终止线程。
- 让 worker 内部提交的子任务走本地队列，降低全局锁竞争。
- 通过工作窃取缓解 worker 间负载不均。
- 明确定义关闭期间已提交、等待中、运行中任务的命运。

## 总体结构

```text
User API
  Submit / SubmitCancelable / Shutdown / ShutdownNow / GetMetrics
        |
        v
Task packaging
  callable + arguments -> packaged_task -> future -> std::function<void()>
        |
        v
Dispatch
  external submit -> bounded global queue
  worker submit   -> owner local queue
  full queue      -> Block / Reject / CallerRuns
        |
        v
Workers
  local pop -> global pop -> steal -> condition-variable wait
        |
        v
Runtime support
  cancellation state / metrics / lifecycle state / task counters
```

核心类型位于 `include/minirt`：

- `ThreadPool`：线程池、任务分发、关闭和指标聚合。
- `ThreadPoolOptions`：线程数、全局队列容量和拒绝策略。
- `RejectionPolicy` 与 `TaskRejected`：背压策略和拒绝异常。
- `WorkStealingQueue<T>`：每个 worker 的本地双端队列。
- `TaskHandle<T>` 与 `CancellationToken`：可取消任务控制。
- `RuntimeMetrics`：原子指标集合与快照。

## 生命周期状态

`ThreadPool` 使用 `RuntimeState` 表达生命周期：

```text
Created -> Running -> Stopping -> Stopped
```

- `Created`：构造过程中，worker 尚未全部启动。
- `Running`：可以接受新任务。
- `Stopping`：关闭已开始，不再接受新任务。
- `Stopped`：worker 已退出并完成 join。

状态不会从 `Stopping` 或 `Stopped` 回到 `Running`。

## 任务包装

`Submit` 使用模板接受任意可调用对象和参数：

```cpp
template<typename F, typename... Args>
auto Submit(F&& function, Args&&... args);
```

提交时会保存独立的函数对象和参数副本：

```cpp
using FunctionType = std::decay_t<F>;
using ArgumentsTuple = std::tuple<std::decay_t<Args>...>;
```

默认语义：

- 普通左值复制到任务内部。
- 右值移动到任务内部。
- 需要引用语义时使用 `std::ref`。

返回类型通过 `std::invoke_result_t` 推导。用户函数被包装为无参 `std::packaged_task<ReturnType()>`，调用者拿到对应的 `std::future<ReturnType>`。最终队列中保存的统一任务类型是：

```cpp
using Task = std::function<void()>;
```

因为 C++17 的 `std::function` 要求目标可复制，而 `std::packaged_task` 是 move-only，实现用 `std::shared_ptr<std::packaged_task<ReturnType()>>` 间接持有 packaged task。

## 调度路径

外部线程提交：

```text
Submit
  -> Dispatch
  -> global queue
  -> worker pop
  -> task()
  -> future shared state
```

worker 内部提交：

```text
worker task
  -> Submit
  -> Dispatch
  -> current worker local queue
  -> owner pop or other worker steal
```

单 worker 线程池中，如果 worker 内部提交子任务后立即等待子任务 future，子任务入队会造成自我等待。因此当前实现对单 worker 内部提交使用 inline CallerRuns 语义。

## 全局队列

外部提交默认进入全局队列：

```cpp
std::queue<Task> m_globalTasks;
```

全局队列由 `m_mutex` 保护，并受 `ThreadPoolOptions::queueCapacity` 限制。容量只限制等待中的外部任务，不包含正在运行的任务、本地队列任务和 CallerRuns 任务。

队列满时按 `RejectionPolicy` 处理：

- `Block`：等待空间，关闭时被唤醒并抛出 `TaskRejected`。
- `Reject`：立即抛出 `TaskRejected`。
- `CallerRuns`：提交线程直接执行任务。

## 本地队列与工作窃取

每个 worker 拥有一个 `WorkStealingQueue<Task>`：

```text
owner Push      -> push_back
owner TryPop    -> pop_back
thief TrySteal  -> pop_front
```

owner 使用 LIFO，有利于继续处理最近生成的子任务。窃取者使用 FIFO，从另一端拿较老任务，减少与 owner 竞争同一个端点。

当前队列基于 `std::deque` 和 `std::mutex`，不是无锁队列。

worker 获取任务顺序：

```text
1. 当前 worker 本地队列
2. 全局队列
3. 其他 worker 本地队列
4. 条件变量等待
```

## 任务计数

`m_pendingTasks` 是原子计数，表示仍在全局队列或本地队列中的任务数。

- 任务入队时加一。
- 任务从队列取出时减一。
- `ShutdownNow()` 丢弃等待任务时按丢弃数量减少。

`m_activeTasks` 表示已经离开队列、正在执行的任务数，包含 worker 正在执行的任务和 CallerRuns 正在执行的任务。它由 `m_mutex` 保护。

```text
pendingTasks: not started yet
activeTasks: started but not finished
```

## 取消模型

`SubmitCancelable` 创建共享取消状态：

```text
TaskHandle.Cancel()
  -> CancellationState.requested = true
  -> CancellationToken observes request
  -> ThrowIfCancellationRequested()
  -> TaskCancelled
  -> packaged_task stores exception
  -> TaskHandle.Get() rethrows
```

取消是协作式的。运行时不会强制终止正在运行的线程，也不会抢占用户代码。

## 指标模型

`RuntimeMetrics` 使用 relaxed 原子计数器。指标用于观测，不参与任务完成同步。

关闭完成后，通常可按以下关系检查任务归宿：

```text
Shutdown:
submitted = completed + failed + cancelled

ShutdownNow:
submitted = completed + failed + cancelled + discarded
```

如果存在提交失败，`rejected` 独立计数，不属于 `submitted`。

## 关键不变量

- 用户任务始终在运行时内部锁之外执行。
- 持有 `m_mutex` 时不调用 `join()`。
- `Shutdown()` 和 `ShutdownNow()` 通过 `m_shutdownMutex` 串行化。
- `Running` 之外的新提交必须拒绝。
- `ShutdownNow()` 只丢弃尚未开始的任务。
- 被丢弃的 packaged task 不会执行，其 future 进入 broken promise。
- worker 退出前会清理 thread-local 身份。
