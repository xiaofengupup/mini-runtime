# MiniRuntime

MiniRuntime 是一个基于 C++17 的轻量级异步任务调度框架。它以固定大小线程池为核心，支持泛型任务提交、`std::future` 结果获取、异常传播、有界队列背压、协作式取消、优雅关闭、立即关闭、运行指标、本地队列和工作窃取。

这个项目更适合用于学习和验证现代 C++ 并发运行时的关键设计，而不是直接替代 oneTBB、Folly、Boost.Asio 等成熟生产级运行时。

## 功能概览

- 固定数量 worker 线程。
- 支持 lambda、普通函数、函数对象、成员函数指针。
- 支持任意返回类型、`void` 返回、move-only 参数和显式引用参数。
- 使用 `std::packaged_task` 与 `std::future` 传递返回值和异常。
- 外部提交进入有界全局队列。
- 队列满时支持 `Block`、`Reject`、`CallerRuns` 三种策略。
- worker 内部提交进入当前 worker 的本地双端队列。
- 空闲 worker 可从其他 worker 本地队列窃取任务。
- `SubmitCancelable` 提供协作式取消。
- `Shutdown()` 优雅排空等待任务。
- `ShutdownNow()` 丢弃尚未开始的等待任务。
- 运行时指标覆盖提交、完成、失败、取消、拒绝、丢弃、CallerRuns、本地提交和窃取。
- GoogleTest 单元测试、压力测试、ASan/UBSan/TSan preset 和轻量基准测试。

## 项目结构

```text
.
├── CMakeLists.txt
├── CMakePresets.json
├── README.md
├── benchmarks/
│   └── benchmark_runtime.cpp
├── cmake/
│   └── Sanitizers.cmake
├── docs/
│   ├── architecture.md
│   ├── optimization.md
│   ├── performance.md
│   ├── shutdown_semantics.md
│   └── thread_model.md
├── include/
│   └── minirt/
│       ├── cancellation.h
│       ├── rejection_policy.h
│       ├── runtime_metrics.h
│       ├── task_handle.h
│       ├── thread_pool.h
│       ├── thread_pool_options.h
│       └── work_stealing_queue.h
├── scripts/
│   └── build_and_test.sh
├── src/
│   └── thread_pool.cpp
└── tests/
    ├── test_backpressure.cpp
    ├── test_cancellation.cpp
    ├── test_metrics.cpp
    ├── test_stress.cpp
    ├── test_submit.cpp
    ├── test_thread_pool.cpp
    ├── test_work_stealing.cpp
    └── test_work_stealing_queue.cpp
```

## 构建要求

- CMake 3.21 或更高版本，用于 `CMakePresets.json`。
- 支持 C++17 的编译器，例如 Apple Clang、Clang 或 GCC。
- POSIX Threads。
- GoogleTest v1.17.0，用于测试构建。

测试 preset 默认优先使用本地源码：

```text
third_party/googletest
```

如果该目录不存在，`scripts/build_and_test.sh` 会清空 preset 中的本地路径覆盖项，让 `CMakeLists.txt` 里的 `FetchContent` 自动下载 GoogleTest。构建环境不能访问网络时，也可以手动准备本地依赖：

```bash
mkdir -p third_party
git clone --branch v1.17.0 --depth 1 https://github.com/google/googletest.git third_party/googletest
```

## 快速开始

运行默认 debug 构建和测试：

```bash
./scripts/build_and_test.sh
```

等价的手动命令：

```bash
cmake --preset debug
cmake --build --preset debug -j
ctest --preset debug
```

运行 sanitizer：

```bash
./scripts/build_and_test.sh asan
./scripts/build_and_test.sh tsan
```

构建 release 并运行基准测试：

```bash
./scripts/build_and_test.sh release --benchmark --threads 4 --tasks 100000
```

## 基本使用

```cpp
#include "minirt/thread_pool.h"

#include <iostream>

int main()
{
    minirt::ThreadPoolOptions options;
    options.threadCount = 4;
    options.queueCapacity = 1024;
    options.rejectionPolicy = minirt::RejectionPolicy::Block;

    minirt::ThreadPool pool(options);

    auto future = pool.Submit([](int lhs, int rhs) {
        return lhs + rhs;
    }, 10, 20);

    std::cout << future.get() << '\n';

    pool.Shutdown();
}
```

输出：

```text
30
```

普通左值会复制到异步任务内部。需要引用语义时显式使用 `std::ref`，并保证被引用对象的生命周期覆盖任务执行。

```cpp
int value = 0;

auto future = pool.Submit([](int& target) {
    target = 42;
}, std::ref(value));

future.get();
```

## 背压策略

外部线程提交的任务进入有界全局队列，容量由 `ThreadPoolOptions::queueCapacity` 控制。

```cpp
options.rejectionPolicy = minirt::RejectionPolicy::Block;
```

`Block` 会在队列满时阻塞提交线程，直到队列出现空间或线程池开始关闭。

```cpp
options.rejectionPolicy = minirt::RejectionPolicy::Reject;
```

`Reject` 会在队列满时抛出 `minirt::TaskRejected`。

```cpp
options.rejectionPolicy = minirt::RejectionPolicy::CallerRuns;
```

`CallerRuns` 会让提交线程直接执行任务，降低继续提交任务的速度。任务返回值和异常仍通过 future 传递。

## 协作式取消

可取消任务的第一个参数必须接收 `minirt::CancellationToken`。

```cpp
auto handle = pool.SubmitCancelable(
    [](minirt::CancellationToken token) {
        while (true) {
            token.ThrowIfCancellationRequested();
            // Process one safe unit of work.
        }
    }
);

handle.Cancel();

try {
    handle.Get();
} catch (const minirt::TaskCancelled&) {
    // Task observed the cancellation request.
}
```

取消是协作式的。`Cancel()` 只设置请求，不会强制终止正在运行的线程。

## 关闭语义

`Shutdown()` 是优雅关闭：

- 停止接受新任务。
- 执行已经排队的全局任务和本地任务。
- 等待正在运行的任务完成。
- join 所有 worker，最终进入 `RuntimeState::Stopped`。

`ShutdownNow()` 是尽快关闭：

- 停止接受新任务。
- 丢弃尚未开始执行的全局任务和本地任务。
- 已经开始运行的任务继续自然结束。
- 被丢弃任务对应的 future 在 `get()` 时抛出 `std::future_error`，错误码为 `std::future_errc::broken_promise`。

两个关闭接口都禁止从当前线程池正在执行的任务内部调用。

## 运行指标

```cpp
const auto metrics = pool.GetMetrics();
```

| 字段 | 含义 |
| --- | --- |
| `submitted` | 被线程池成功接受的任务数 |
| `completed` | 正常完成的任务数 |
| `failed` | 用户任务抛出普通异常的任务数 |
| `cancelled` | 任务抛出 `TaskCancelled` 的任务数 |
| `rejected` | 提交阶段被拒绝的任务数 |
| `discarded` | `ShutdownNow()` 丢弃的等待任务数 |
| `callerRuns` | 由提交线程直接执行的任务数 |
| `localSubmitted` | worker 内部提交到本地队列的任务数 |
| `stolen` | 被其他 worker 成功窃取的任务数 |

指标使用 relaxed 原子计数，适合观测，不应替代 future、条件变量或业务级同步。

## 基准测试

Release 构建后运行：

```bash
cmake --preset release
cmake --build --preset release -j
./build/release/minirt_benchmark 4 100000
```

当前 benchmark 覆盖两类场景：

- `external submission`：外部线程持续提交小任务。
- `nested work stealing`：worker 内部批量提交子任务，触发本地队列和工作窃取。

更多性能分析和改进建议见 [docs/performance.md](docs/performance.md) 与 [docs/optimization.md](docs/optimization.md)。

## 文档索引

- [docs/architecture.md](docs/architecture.md)：整体架构、任务包装、调度流程和关键不变量。
- [docs/thread_model.md](docs/thread_model.md)：线程角色、锁职责、worker 身份和执行模型。
- [docs/shutdown_semantics.md](docs/shutdown_semantics.md)：优雅关闭、立即关闭、并发关闭和 future 行为。
- [docs/performance.md](docs/performance.md)：基准测试方法、结果解读和性能特征。
- [docs/optimization.md](docs/optimization.md)：基于当前实现的优化清单和优先级。
