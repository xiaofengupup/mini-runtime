# MiniRuntime 架构设计

## 1. 项目目标

MiniRuntime 是一个基于 C++17 实现的轻量级多线程任务运行时，用于学习和实践现代 C++ 泛型编程、线程同步、任务调度、生命周期管理、背压、协作式取消和工作窃取。

项目主要解决以下问题：

* 如何将不同类型的可调用对象统一提交到线程池；
* 如何跨线程传递任务返回值和异常；
* 如何安全管理工作线程的启动和退出；
* 如何避免无限任务队列导致内存持续增长；
* 如何在线程池过载时向提交者反馈压力；
* 如何在不强制终止线程的情况下请求任务取消；
* 如何降低所有工作线程竞争单一全局队列的开销；
* 如何验证并发程序不存在任务丢失、重复执行和生命周期错误。

MiniRuntime 不是完整的生产级并发运行时，但实现了一个现代任务调度框架的核心组成部分。

---

## 2. 整体架构

```text
┌──────────────────────────────────────────────────┐
│                    User API                      │
│                                                  │
│ Submit                                           │
│ SubmitCancelable                                 │
│ Shutdown                                         │
│ ShutdownNow                                      │
│ GetMetrics                                       │
└────────────────────────┬─────────────────────────┘
                         │
                         ▼
┌──────────────────────────────────────────────────┐
│                  Task Packaging                  │
│                                                  │
│ Callable + Arguments                             │
│ std::packaged_task                               │
│ std::future                                      │
│ Exception propagation                            │
│ Type erasure to std::function<void()>            │
└────────────────────────┬─────────────────────────┘
                         │
                         ▼
┌──────────────────────────────────────────────────┐
│                    Dispatch                      │
│                                                  │
│ External submission → Global bounded queue       │
│ Worker submission   → Worker local queue         │
│ Queue full          → Block / Reject / CallerRuns│
└────────────────────────┬─────────────────────────┘
                         │
                         ▼
┌──────────────────────────────────────────────────┐
│                     Workers                      │
│                                                  │
│ 1. Pop local task                                │
│ 2. Pop global task                               │
│ 3. Steal from another worker                     │
│ 4. Wait on condition variable                    │
└────────────────────────┬─────────────────────────┘
                         │
                         ▼
┌──────────────────────────────────────────────────┐
│               Runtime Support                    │
│                                                  │
│ CancellationToken                                │
│ TaskHandle                                       │
│ RuntimeMetrics                                   │
│ Lifecycle state machine                          │
│ Pending and active task tracking                 │
└──────────────────────────────────────────────────┘
```

---

## 3. 核心组件

### 3.1 `ThreadPool`

`ThreadPool` 是运行时的核心对象，负责：

* 创建和回收工作线程；
* 接收任务；
* 将任务分发到全局或本地队列；
* 管理线程池状态；
* 实现关闭语义；
* 维护待执行任务和正在执行任务的数量；
* 维护运行指标；
* 协调工作线程休眠和唤醒。

主要接口：

```cpp
explicit ThreadPool(std::size_t threadCount);
explicit ThreadPool(ThreadPoolOptions options);

template <typename F, typename... Args>
auto Submit(F&& function, Args&&... args);

template <typename F, typename... Args>
auto SubmitCancelable(F&& function, Args&&... args);

void Shutdown();
void ShutdownNow();

RuntimeState GetState() const;
std::size_t PendingTaskCount() const noexcept;
RuntimeMetricsSnapshot GetMetrics() const noexcept;
```

---

### 3.2 `ThreadPoolOptions`

线程池配置：

```cpp
struct ThreadPoolOptions {
    std::size_t threadCount;
    std::size_t queueCapacity;
    RejectionPolicy rejectionPolicy;
};
```

字段含义：

* `threadCount`：工作线程数量；
* `queueCapacity`：全局等待队列容量；
* `rejectionPolicy`：全局队列满时的处理策略。

本地工作队列当前不受 `queueCapacity` 限制。

---

### 3.3 `WorkStealingQueue`

每个工作线程拥有一个本地双端队列。

内部使用：

```text
std::deque<Task>
std::mutex
```

队列操作规则：

```text
Owner worker:
    Push      → push_back
    TryPop    → pop_back

Other worker:
    TrySteal  → pop_front
```

本地 Worker 使用 LIFO，有利于优先处理最近生成的子任务。

窃取线程使用 FIFO，从另一端获取较老任务，减少与本地 Worker 的直接竞争。

当前实现基于互斥锁，不是无锁队列。

---

### 3.4 `TaskHandle<T>`

`TaskHandle<T>` 用于控制可取消任务。

内部包含：

```cpp
std::future<T>
std::shared_ptr<CancellationState>
```

主要能力：

```cpp
void Cancel();
bool IsCancellationRequested() const;
bool Valid() const;

template <typename Rep, typename Period>
std::future_status WaitFor(...);

T Get();
```

`TaskHandle` 不可复制，但可以移动，因为 `std::future` 是 move-only 类型。

---

### 3.5 `CancellationToken`

`CancellationToken` 传入可取消任务。

任务可以主动调用：

```cpp
token.IsCancellationRequested();
token.ThrowIfCancellationRequested();
```

取消是协作式的。运行时不会强制终止工作线程。

---

### 3.6 `RuntimeMetrics`

运行时维护以下指标：

```text
submitted
completed
failed
cancelled
rejected
discarded
callerRuns
localSubmitted
stolen
```

这些指标使用原子计数器，并采用 `memory_order_relaxed`。

指标只用于观测，不用于任务完成同步。

---

## 4. 任务提交链路

普通任务提交示例：

```cpp
auto future = pool.Submit(
    [](int lhs, int rhs) {
        return lhs + rhs;
    },
    10,
    20
);
```

完整调用链：

```text
用户调用 Submit
      ↓
保存 callable 和 arguments
      ↓
生成无参数 user task
      ↓
包装为 std::packaged_task<ReturnType()>
      ↓
取得 std::future<ReturnType>
      ↓
包装为 std::function<void()>
      ↓
Dispatch
      ↓
全局队列 / 本地队列 / CallerRuns
      ↓
Worker 执行 packaged_task
      ↓
返回值或异常写入共享状态
      ↓
future.get() 获取结果或重新抛出异常
```

---

## 5. 泛型任务包装

### 5.1 参数保存

异步任务不能长期保存调用者栈上的转发引用，因此提交时会保存函数和参数的独立副本。

```cpp
using FunctionType = std::decay_t<F>;
using ArgumentsTuple =
    std::tuple<std::decay_t<Args>...>;
```

默认语义：

```text
普通左值 → 复制
右值     → 移动
std::ref → 显式引用
```

如果任务需要修改外部对象，应显式使用：

```cpp
std::ref(object)
```

调用者必须保证被引用对象在任务执行期间仍然有效。

---

### 5.2 返回类型推导

返回类型通过：

```cpp
std::invoke_result_t
```

推导。

MiniRuntime 支持：

* `void`；
* 基本类型；
* 标准库类型；
* 用户自定义类型；
* move-only 参数；
* 普通函数；
* lambda；
* 函数对象；
* 成员函数指针。

---

### 5.3 `packaged_task` 和 `future`

`std::packaged_task` 负责：

* 执行用户函数；
* 捕获返回值；
* 捕获用户异常；
* 将结果写入共享状态。

`std::future` 从同一个共享状态读取结果。

```text
Worker thread
    │
    │ packaged_task writes
    ▼
Shared state
    ▲
    │ future.get() reads
    │
Caller thread
```

如果用户任务抛出异常，工作线程不会因为该异常退出。

异常被保存到共享状态，并在：

```cpp
future.get();
```

时重新抛出。

---

## 6. 类型擦除

不同任务的实际类型可能完全不同。

例如：

```cpp
[] { return 1; }
[](std::string value) { return value.size(); }
SomeFunctionObject{}
```

经过任务包装后，最终统一存储为：

```cpp
using Task = std::function<void()>;
```

工作线程只需要执行：

```cpp
task();
```

不需要了解任务原始参数和返回类型。

由于 C++17 的 `std::function` 要求内部对象可复制，而 `std::packaged_task` 不可复制，因此使用：

```cpp
std::shared_ptr<std::packaged_task<ReturnType()>>
```

间接持有 `packaged_task`。

---

## 7. 调度策略

### 7.1 外部线程提交

外部线程提交的任务进入全局有界队列：

```text
External thread
      ↓
Global bounded queue
```

队列满时根据配置执行：

* `Block`
* `Reject`
* `CallerRuns`

---

### 7.2 工作线程内部提交

工作线程执行任务期间再次提交任务时，任务进入当前 Worker 的本地队列：

```text
Worker N
   ↓
Local queue N
```

这样可以降低嵌套任务反复访问全局队列造成的锁竞争。

如果线程池只有一个 Worker，内部提交会直接内联执行，避免父任务等待子任务时发生自我死锁。

---

### 7.3 Worker 获取任务顺序

```text
1. 当前 Worker 的本地队列
2. 全局队列
3. 其他 Worker 的本地队列
4. 条件变量等待
```

优先级体现了以下目标：

* 优先利用本地缓存；
* 继续处理外部提交任务；
* 在负载不均时窃取任务；
* 没有任务时避免忙等。

---

## 8. 工作窃取

假设 Worker 0 的本地队列有大量任务，而 Worker 1 空闲：

```text
Worker 0 local queue:
[A, B, C, D]

Worker 1 local queue:
[]
```

Worker 1 会执行：

```text
TryPopLocal(1)
    ↓ failed
TryPopGlobal()
    ↓ failed
TrySteal(1)
    ↓ steal A from Worker 0
```

窃取成功时：

* `pendingTasks` 减一；
* `stolen` 指标加一；
* 任务转为 active；
* Worker 执行任务。

工作窃取可以改善负载不均，但不保证在所有场景下都更快。

---

## 9. 待执行任务与活跃任务

### 9.1 `pendingTasks`

`pendingTasks` 表示仍位于某个队列中的任务数量，包括：

* 全局队列；
* 所有本地队列。

任务进入任意队列时加一。

任务从任意队列取出或被 `ShutdownNow()` 丢弃时减一。

---

### 9.2 `activeTasks`

`activeTasks` 表示已经离开队列、正在执行的任务数量，包括：

* Worker 正在执行的任务；
* CallerRuns 执行的任务。

`pendingTasks` 和 `activeTasks` 含义不同：

```text
pendingTasks → 尚未开始
activeTasks  → 已经开始但尚未结束
```

---

## 10. 状态机

线程池生命周期：

```text
Created
   ↓
Running
   ↓
Stopping
   ↓
Stopped
```

### `Created`

对象正在构造，工作线程尚未完全启动。

### `Running`

可以接收新任务。

### `Stopping`

不再接收新任务，正在排空或丢弃等待任务。

### `Stopped`

所有工作线程已经退出并被 `join()`。

状态不会从 `Stopping` 或 `Stopped` 返回 `Running`。

---

## 11. 条件变量

MiniRuntime 使用多个条件变量表达不同等待条件。

### `taskAvailableCv`

工作线程等待：

```text
存在待执行任务
或者
线程池开始关闭
```

### `queueNotFullCv`

Block 策略提交线程等待：

```text
全局队列出现空间
或者
线程池开始关闭
```

### `idleCv`

关闭线程等待：

```text
activeTasks == 0
```

条件变量只负责通知。实际条件必须由谓词重新检查，以处理虚假唤醒。

---

## 12. 取消机制

取消状态由 `TaskHandle` 和 `CancellationToken` 共享：

```text
TaskHandle.Cancel()
        ↓
CancellationState.requested = true
        ↓
CancellationToken observes state
        ↓
ThrowIfCancellationRequested()
        ↓
TaskCancelled
        ↓
packaged_task stores exception
        ↓
TaskHandle.Get() rethrows
```

调用 `Cancel()` 只代表发出取消请求，不代表任务已经停止。

任务可能：

* 在开始前观察取消；
* 在运行中观察取消；
* 完全不检查 Token，并正常完成。

---

## 13. 并发不变量

MiniRuntime 的核心正确性依赖以下不变量：

### 13.1 单一任务位置

任意时刻，一个任务只能处于以下一个位置：

```text
全局队列
某个本地队列
正在执行
已完成
已失败
已取消
已丢弃
```

任务不能同时位于多个队列，也不能被执行两次。

### 13.2 成功接受的任务必须有明确结果

成功提交的任务最终只能：

* 正常完成；
* 抛出用户异常；
* 响应协作式取消；
* 被 `ShutdownNow()` 丢弃并产生 broken promise。

### 13.3 不能持有运行时主锁执行用户任务

用户任务必须在运行时内部锁之外执行。

### 13.4 不能持有运行时主锁调用 `join()`

Worker 退出和任务完成都可能需要获取运行时主锁。

持锁调用 `join()` 会导致死锁。

---

## 14. 测试与验证

项目使用 GoogleTest 覆盖：

* 泛型任务提交；
* 返回值和异常传播；
* 生命周期；
* 并发关闭；
* 有界队列；
* 三种拒绝策略；
* 协作式取消；
* 运行指标；
* 本地队列；
* 工作窃取；
* 大量嵌套任务；
* 多生产者与关闭竞争；
* 每个任务恰好执行一次。

此外使用：

* AddressSanitizer；
* UndefinedBehaviorSanitizer；
* ThreadSanitizer；
* Release benchmark。

---

## 15. 已知限制

当前实现存在以下限制：

* 本地队列不是无锁结构；
* 本地队列没有容量上限；
* 窃取目标采用固定轮询；
* 没有批量窃取；
* 没有任务优先级；
* 没有定时任务；
* 没有 DAG 依赖；
* 没有 C++20 coroutine 适配；
* 不支持任务内部关闭所属线程池；
* 协作式取消依赖任务主动检查；
* `ShutdownNow()` 通过 broken promise 表示被丢弃任务；
* 工作窃取无法解决循环依赖；
* 多个父任务同步等待子任务时仍可能出现线程池饥饿。
