# MiniRuntime

一个基于 C++17 实现的轻量级多线程任务运行时。

MiniRuntime 支持泛型任务提交、返回值与异常传播、有界任务队列、背压策略、协作式取消、优雅关闭、立即关闭、运行指标、每线程本地队列和工作窃取调度。

该项目的主要目标是学习和实践现代 C++ 并发编程、任务调度及工程化测试，而不是替代 oneTBB、Folly 或成熟生产级运行时。

---

## 核心功能

* 固定大小工作线程池；
* 支持 lambda、普通函数和函数对象；
* 支持任意参数和返回类型；
* 支持 `void` 任务；
* 支持 move-only 参数；
* 使用 `std::future` 获取结果；
* 使用 `std::packaged_task` 传播异常；
* 明确的生命周期状态机；
* 有界全局任务队列；
* `Block`、`Reject`、`CallerRuns` 三种背压策略；
* 协作式任务取消；
* 超时等待；
* 每 Worker 本地双端队列；
* 基于互斥锁的工作窃取；
* `Shutdown()` 和 `ShutdownNow()`；
* 运行指标；
* GoogleTest 单元测试和压力测试；
* ASan、UBSan 和 TSan；
* Release 性能基准。

---

## 项目结构

```text
MiniRuntime/
├── CMakeLists.txt
├── CMakePresets.json
├── README.md
├── cmake/
│   └── Sanitizers.cmake
├── include/
│   └── minirt/
│       ├── cancellation.h
│       ├── rejection_policy.h
│       ├── runtime_metrics.h
│       ├── task_handle.h
│       ├── thread_pool.h
│       ├── thread_pool_options.h
│       └── work_stealing_queue.h
├── src/
│   └── thread_pool.cpp
├── tests/
│   ├── test_thread_pool.cpp
│   ├── test_submit.cpp
│   ├── test_backpressure.cpp
│   ├── test_cancellation.cpp
│   ├── test_metrics.cpp
│   ├── test_work_stealing_queue.cpp
│   ├── test_work_stealing.cpp
│   └── test_stress.cpp
├── benchmarks/
│   └── benchmark_runtime.cpp
├── third_party/
│   └── googletest/
└── docs/
    ├── architecture.md
    ├── thread_model.md
    ├── shutdown_semantics.md
    └── performance.md
```

---

## 架构概览

```text
┌──────────────────────────────────────┐
│               User API               │
│ Submit / Cancel / Wait / Shutdown    │
└──────────────────┬───────────────────┘
                   │
                   ▼
┌──────────────────────────────────────┐
│             Task Packaging           │
│ packaged_task / future / type erase  │
└──────────────────┬───────────────────┘
                   │
                   ▼
┌──────────────────────────────────────┐
│                Dispatch              │
│                                      │
│ External → Global bounded queue      │
│ Worker   → Local work queue          │
│ Full     → Block/Reject/CallerRuns   │
└──────────────────┬───────────────────┘
                   │
                   ▼
┌──────────────────────────────────────┐
│                Workers               │
│ Local → Global → Steal → Wait        │
└──────────────────┬───────────────────┘
                   │
                   ▼
┌──────────────────────────────────────┐
│ Future / Cancellation / Metrics      │
└──────────────────────────────────────┘
```

详细设计参见：

* `docs/architecture.md`
* `docs/thread_model.md`
* `docs/shutdown_semantics.md`

---

## 环境要求

* CMake 3.21 或更高版本；
* 支持 C++17 的编译器；
* Apple Clang、Clang 或 GCC；
* POSIX Threads；
* GoogleTest 1.17.0。

项目默认将 GoogleTest 源码放在：

```text
third_party/googletest
```

不同构建 Preset 会分别编译 GoogleTest，但不需要重复下载源码。

---

## 获取 GoogleTest

首次执行：

```bash
mkdir -p third_party

git clone \
    --branch v1.17.0 \
    --depth 1 \
    https://github.com/google/googletest.git \
    third_party/googletest
```

也可以使用 Git submodule：

```bash
git submodule add \
    https://github.com/google/googletest.git \
    third_party/googletest

cd third_party/googletest
git checkout v1.17.0
cd ../..
```

克隆带 submodule 的项目：

```bash
git clone --recurse-submodules <repository-url>
```

---

## 快速构建

### Debug

```bash
cmake --preset debug
cmake --build --preset debug -j
ctest --preset debug
```

### Release

```bash
cmake --preset release
cmake --build --preset release -j
```

---

## 基本使用

```cpp
#include "minirt/thread_pool.h"

#include <iostream>

int main() {
    minirt::ThreadPoolOptions options;

    options.threadCount = 4;
    options.queueCapacity = 1024;
    options.rejectionPolicy =
        minirt::RejectionPolicy::Block;

    minirt::ThreadPool pool(options);

    auto future = pool.Submit(
        [](int lhs, int rhs) {
            return lhs + rhs;
        },
        10,
        20
    );

    std::cout << future.get() << '\n';

    pool.Shutdown();

    return 0;
}
```

输出：

```text
30
```

> 如果你的实际成员命名使用 `thread_count`、`queue_capacity` 和 `rejection_policy`，请按照当前头文件命名调整示例。

---

## 返回值与异常传播

任务返回值通过 `std::future` 获取：

```cpp
auto future = pool.Submit([] {
    return std::string("MiniRuntime");
});

std::cout << future.get();
```

任务异常会被保存到 Future 的共享状态：

```cpp
auto future = pool.Submit([]() -> int {
    throw std::runtime_error("task failed");
});

try {
    future.get();
} catch (const std::runtime_error& exception) {
    std::cerr << exception.what() << '\n';
}
```

某个任务抛出异常不会导致 Worker 线程退出。

---

## 参数传递

普通左值默认复制到任务内部：

```cpp
std::string value = "input";

auto future = pool.Submit(
    [](std::string argument) {
        return argument.size();
    },
    value
);
```

右值会移动到任务内部：

```cpp
auto future = pool.Submit(
    [](std::unique_ptr<int> value) {
        return *value;
    },
    std::make_unique<int>(42)
);
```

需要显式引用语义时使用：

```cpp
std::ref
```

```cpp
int value = 0;

auto future = pool.Submit(
    [](int& target) {
        target = 42;
    },
    std::ref(value)
);

future.get();
```

调用者必须保证被引用对象的生命周期覆盖任务执行时间。

---

## 有界队列与背压

MiniRuntime 的外部任务进入有界全局队列。

当任务提交速度持续高于执行速度时，队列最终会达到容量上限。

支持三种策略。

### Block

```cpp
options.rejectionPolicy =
    minirt::RejectionPolicy::Block;
```

队列满时阻塞提交线程，直到出现空间或线程池关闭。

### Reject

```cpp
options.rejectionPolicy =
    minirt::RejectionPolicy::Reject;
```

队列满时抛出：

```cpp
minirt::TaskRejected
```

### CallerRuns

```cpp
options.rejectionPolicy =
    minirt::RejectionPolicy::CallerRuns;
```

队列满时由提交线程直接执行任务。

CallerRuns 会降低提交线程继续生产任务的速度，因此是一种自然背压机制。

---

## 协作式取消

提交可取消任务：

```cpp
auto handle = pool.SubmitCancelable(
    [](minirt::CancellationToken token) {
        while (true) {
            token.ThrowIfCancellationRequested();

            // Process one safe unit of work.
        }
    }
);
```

请求取消：

```cpp
handle.Cancel();
```

等待结果：

```cpp
try {
    handle.Get();
} catch (const minirt::TaskCancelled&) {
    // Task observed the cancellation request.
}
```

取消是协作式的。

`Cancel()` 只设置取消请求。任务必须主动检查 Token。

不检查 Token 的任务不会被强制停止。

---

## 超时等待

```cpp
if (
    handle.WaitFor(std::chrono::seconds(1)) ==
    std::future_status::timeout
) {
    handle.Cancel();
}
```

超时只表示调用线程停止等待，不会自动终止后台任务。

---

## 工作窃取

外部线程提交的任务进入全局队列。

Worker 内部生成的子任务进入该 Worker 的本地队列。

Worker 获取任务顺序：

```text
Local queue
    ↓
Global queue
    ↓
Steal from another worker
    ↓
Wait
```

本地 Worker 从队尾取任务，其他 Worker 从队头窃取任务。

```text
Owner:
    push_back
    pop_back

Thief:
    pop_front
```

当前本地队列使用 `mutex + deque`，不是无锁结构。

---

## 线程池关闭

### 优雅关闭

```cpp
pool.Shutdown();
```

行为：

* 停止接受新任务；
* 执行全部全局等待任务；
* 执行全部本地等待任务；
* 等待正在运行的任务结束；
* 回收所有 Worker。

### 立即关闭

```cpp
pool.ShutdownNow();
```

行为：

* 停止接受新任务；
* 丢弃全局等待任务；
* 丢弃本地等待任务；
* 已经开始的任务继续运行；
* 等待 Worker 自然退出。

被丢弃任务的 Future 会抛出：

```cpp
std::future_error
```

错误码为：

```cpp
std::future_errc::broken_promise
```

`ShutdownNow()` 不会强制杀死正在运行的线程。

---

## 运行指标

获取快照：

```cpp
const auto metrics = pool.GetMetrics();
```

指标包括：

| 字段               | 含义                       |
| ---------------- | ------------------------ |
| `submitted`      | 成功接受的任务数                 |
| `completed`      | 正常完成任务数                  |
| `failed`         | 以普通异常结束的任务数              |
| `cancelled`      | 响应取消的任务数                 |
| `rejected`       | 提交阶段被拒绝的任务数              |
| `discarded`      | 被 `ShutdownNow()` 丢弃的任务数 |
| `callerRuns`     | 由提交线程执行的任务数              |
| `localSubmitted` | 进入本地队列的任务数               |
| `stolen`         | 成功窃取的任务数                 |

指标是观测数据，不应被用于代替 Future 或条件变量进行任务同步。

---

## 执行测试

运行全部测试：

```bash
ctest \
    --test-dir build/debug \
    --output-on-failure
```

直接运行 GoogleTest：

```bash
./build/debug/minirt_tests
```

只运行指定测试：

```bash
./build/debug/minirt_tests \
    --gtest_filter='WorkStealingTest.*'
```

重复执行：

```bash
./build/debug/minirt_tests \
    --gtest_repeat=100 \
    --gtest_break_on_failure
```

---

## Sanitizer

### AddressSanitizer 和 UndefinedBehaviorSanitizer

```bash
cmake --preset asan
cmake --build --preset asan -j
ctest --preset asan
```

直接运行：

```bash
ASAN_OPTIONS=abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
./build/asan/minirt_tests
```

### ThreadSanitizer

```bash
cmake --preset tsan
cmake --build --preset tsan -j
ctest --preset tsan
```

直接运行：

```bash
TSAN_OPTIONS=halt_on_error=1 \
./build/tsan/minirt_tests
```

TSan 必须使用独立构建目录，不能与 ASan 混合。

---

## 性能基准

使用 Release 构建：

```bash
cmake --preset release
cmake --build --preset release -j
```

运行：

```bash
./build/release/minirt_benchmark 4 100000
```

参数：

```text
第一个参数：工作线程数量
第二个参数：任务数量
```

建议测试：

```bash
./build/release/minirt_benchmark 1 100000
./build/release/minirt_benchmark 2 100000
./build/release/minirt_benchmark 4 100000
./build/release/minirt_benchmark 8 100000
```

基准包含：

* 外部任务提交；
* Worker 内部嵌套提交；
* 本地队列与工作窃取。

性能结果应在相同机器和相同 Release 配置下多次执行，并记录中位数。

---

## 并发设计要点

项目遵守以下核心规则：

1. 用户任务必须在运行时内部锁之外执行；
2. 不能持有运行时主锁调用 `join()`；
3. 状态检查和全局任务入队必须位于同一个临界区；
4. 任务从任意队列取出后必须更新 `pendingTasks`；
5. 每个成功提交的任务最多执行一次；
6. 所有工作线程必须在对象成员销毁前完成 `join()`；
7. 取消是请求，不是强制终止；
8. 条件变量必须配合谓词；
9. 工作窃取一次只锁一个本地队列；
10. Worker 启动前必须完成所有本地队列初始化。

---

## 已知限制

当前版本存在以下限制：

* 本地队列使用互斥锁，不是无锁队列；
* 本地队列没有容量上限；
* 窃取目标采用固定轮询；
* 没有随机 victim 选择；
* 没有批量窃取；
* 没有任务优先级；
* 没有定时任务；
* 没有任务依赖和 DAG；
* 没有 C++20 coroutine 接口；
* 不支持任务内部关闭所属线程池；
* 协作式取消依赖任务主动检查；
* `ShutdownNow()` 使用 broken promise 表示丢弃；
* 工作窃取不能解决循环依赖；
* 多个父任务同步等待子任务时仍可能出现线程池饥饿；
* 尚未针对 NUMA 和 CPU affinity 优化。

---

## 后续计划

可能的后续扩展：

* 有界本地队列；
* 随机化窃取目标；
* 批量窃取；
* 任务优先级；
* 定时任务和最小堆定时器；
* TaskGroup；
* DAG 依赖调度；
* move-only 任务类型擦除；
* C++20 `std::stop_token` 适配；
* C++20 coroutine adapter；
* 无锁工作窃取双端队列；
* tracing 和延迟分布；
* NUMA 感知调度。

---

## 项目定位

MiniRuntime 重点展示以下能力：

* C++17 泛型编程；
* 完美转发和移动语义；
* RAII；
* 类型擦除；
* `packaged_task` 和 `future`；
* mutex、condition variable 和 atomic；
* 生命周期状态机；
* 背压和拒绝策略；
* 协作式取消；
* 工作窃取；
* 并发压力测试；
* Sanitizer；
* CMake 工程化。

---

## 简历描述

> **MiniRuntime——现代 C++ 多线程任务调度框架**
> 基于 C++17 实现支持泛型任务提交、返回值与异常传播、有界队列、背压策略、优雅关闭及协作式取消的异步任务运行时；设计每线程本地双端队列和基于互斥锁的工作窃取调度，降低嵌套任务中的全局队列竞争，并通过 GoogleTest、并发压力测试、ASan、UBSan、TSan 与 Release benchmark 验证运行时正确性和可扩展性。

---