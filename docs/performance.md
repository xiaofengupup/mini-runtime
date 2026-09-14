# MiniRuntime 性能说明

当前仓库提供一个轻量级 benchmark：

```text
benchmarks/benchmark_runtime.cpp
```

它使用 `std::chrono::steady_clock` 计时，不依赖 Google Benchmark。这个基准适合观察趋势和回归，不适合作为严格的跨机器性能结论。

## 运行方式

推荐使用 Release 构建：

```bash
cmake --preset release
cmake --build --preset release -j
./build/release/minirt_benchmark --threads 4 --tasks 100000 --task-type light --iterations 5 --warmup 1
```

常用参数：

```text
--threads N       Worker 线程数
--tasks N         每轮任务数
--task-type TYPE  任务粒度：empty、light、medium、heavy，也可使用 0、1、2、3
--iterations N    正式测量轮数
--warmup N        预热轮数
```

也可以通过脚本运行：

```bash
./scripts/build_and_test.sh --preset release --benchmark --threads 4 --tasks 100000 --task-type light --iterations 5 --warmup 1
```

## 当前场景

### external submission

外部线程批量提交小任务，每个任务返回一个整数计算结果。计时范围包含：

- `Submit` 模板包装成本。
- `std::packaged_task` 和 `std::future` 成本。
- `std::shared_ptr` 分配和引用计数成本。
- 全局队列锁竞争。
- worker 唤醒和出队成本。
- future 收集结果成本。
- `Shutdown()` 成本。

该场景主要观察外部生产者向有界全局队列投递小任务时的吞吐。

### nested work stealing

先提交一个外层任务，外层任务在 worker 内部批量提交子任务。子任务进入该 worker 的本地队列，其他 worker 空闲时从本地队列窃取。

计时范围包含：

- 内部提交成本。
- 本地队列 `Push` / `TryPop` 成本。
- 多 worker 窃取成本。
- 父任务等待所有子任务 future 的成本。

该场景主要观察本地队列和工作窃取是否能让嵌套任务完成。

## 结果解读

历史文档中的一组结果显示，线程数增加后吞吐没有线性提升，甚至下降：

| Threads | Scenario | Tasks | Time | Throughput |
| --- | --- | --- | --- | --- |
| 1 | External submission | 100000 | 0.0483s | 2069313 tasks/s |
| 2 | External submission | 100000 | 0.0998s | 1002183 tasks/s |
| 4 | External submission | 100000 | 0.1112s | 899416 tasks/s |
| 8 | External submission | 100000 | 0.2577s | 388103 tasks/s |
| 1 | Nested work stealing | 100000 | 0.0123s | 8146419 tasks/s |
| 2 | Nested work stealing | 100000 | 0.0246s | 4062433 tasks/s |
| 4 | Nested work stealing | 100000 | 0.0612s | 1633424 tasks/s |
| 8 | Nested work stealing | 100000 | 0.2108s | 474363 tasks/s |

这类结果并不意外。benchmark 中的任务非常小，调度开销、future 共享状态、堆分配、原子指标和锁竞争占比远高于真实计算本身。线程越多，竞争和唤醒成本越明显。

## 主要开销来源

- 每个任务至少涉及一次 callable/arguments 保存。
- 每个任务创建一个 `std::packaged_task` 和一个 future 共享状态。
- 为了放入 `std::function<void()>`，实现使用 `shared_ptr<packaged_task>`。
- 外部提交集中竞争 `m_mutex` 保护的全局队列。
- worker 空闲路径会按顺序检查本地队列、全局队列和其他本地队列。
- 工作窃取基于 mutex，在高线程数下会产生额外锁竞争。
- 指标更新虽然使用 relaxed 原子，但仍然是每任务热路径上的写操作。
- `notify_one` / `condition_variable` 唤醒存在系统调度成本。

## 适合的任务粒度

MiniRuntime 更适合任务本身有一定工作量的场景，例如：

- CPU 计算片段。
- 可并行处理的数据块。
- 批处理中的独立步骤。
- 不频繁阻塞的后台任务。

如果任务只是几个整数运算，调度成本会主导总耗时。此时更好的做法通常是批量化任务，让单个异步任务处理一段数据，而不是提交大量极小任务。

## Benchmark 可改进点

当前 benchmark 可改进为更可靠的性能观察工具：

- 增加多轮运行，报告 min/median/p95，而不是单次结果。
- 将预热结果丢弃，并对每个场景单独预热。
- 将 submit 阶段、execute 阶段、future collect 阶段拆开计时。
- 增加不同任务粒度，例如空任务、小计算、中等计算。
- 增加不同 `queueCapacity` 和 `RejectionPolicy` 对比。
- 增加多外部提交线程场景，模拟真实生产者竞争。
- 输出 CSV 或 JSON，方便长期跟踪。
- 对 nested 场景报告 `localSubmitted` 和 `stolen` 指标。
- 避免把 `Shutdown()` 成本混入主吞吐指标，或单独报告。

更完整的工程优化建议见 [optimization.md](optimization.md)。
