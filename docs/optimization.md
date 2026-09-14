# MiniRuntime 优化建议

本文基于当前源码和 benchmark 设计，整理 MiniRuntime 后续可以优化的方向。建议按收益、风险和实现复杂度分阶段推进。

## 总体判断

当前实现的优点是语义清晰、测试覆盖较完整、关闭模型明确。主要性能瓶颈来自小任务场景中的固定调度成本：

- `std::packaged_task`、future 共享状态和 `shared_ptr` 分配。
- 外部提交集中竞争单个全局队列锁。
- 每个任务都会更新多个原子指标。
- 工作窃取队列使用 mutex，高并发下会出现额外竞争。
- nested benchmark 中父任务同步等待大量子任务 future，调度开销被放大。

优化应优先保持语义正确性，尤其是关闭、取消、异常传播和 future 行为。

## 优先级 P0：测量体系

在改调度器之前，先改 benchmark。否则很难判断优化是真收益还是噪声。

建议：

- [x] 每个场景运行多轮，输出 min、median、p95。
- [ ] ~~支持 `--csv` 或 `--json` 输出。~~
- [x] 分离 submit time、wait/get time、shutdown time。
- [x] 增加任务粒度参数，例如空任务、轻计算、中计算。
- [x] 增加多生产者外部提交场景。
- [x] 在 nested 场景输出 `localSubmitted`、`stolen`、`callerRuns`。
- [x] 固定线程数序列，例如 `1,2,4,8,hardware_concurrency`。

收益：低风险、高确定性。它不会改变运行时行为，但能让后续优化有依据。

## 优先级 P1：减少任务包装成本

当前队列类型是：

```cpp
using Task = std::function<void()>;
```

为了把 move-only 的 `std::packaged_task` 放入 `std::function`，实现使用 `shared_ptr<packaged_task>`。这会带来额外堆分配和引用计数。

可选优化：

- C++23 可改用 `std::move_only_function<void()>`。
- C++17 可自定义 move-only task wrapper。
- 对无返回值 fire-and-forget 任务提供专门接口，避免 future 共享状态。
- 引入任务对象池，复用包装节点。

风险：中等。需要完整验证异常传播、取消、`ShutdownNow()` 丢弃任务后的 broken promise 行为。

## 优先级 P1：改进全局提交路径

外部提交统一进入 `m_globalTasks`，由 `m_mutex` 保护。多生产者和多 worker 场景下，这会成为热点。

可选优化：

- 分片全局队列，按提交线程 hash 或 round-robin 分发。
- 外部提交也按 worker round-robin 投递到本地队列，但需要重新设计容量限制。
- 使用 MPMC 有界队列替换 `std::queue + mutex`。
- 将状态锁和全局队列锁拆开，减少无关临界区竞争。

风险：中到高。容量、Block 唤醒、`ShutdownNow()` 清理和 `pendingTasks` 计数都会受影响。

## 优先级 P1：完善工作窃取策略

当前 worker 窃取顺序固定：

```text
(workerIndex + 1) % workerCount
```

固定顺序简单，但高并发下多个空闲 worker 可能集中访问同一批 victim。

可选优化：

- 为每个 worker 引入伪随机 victim 起点。
- 记录上次成功窃取的 victim，下次从附近开始。
- 空闲失败后短暂退避，降低无任务时的锁探测频率。
- 对本地队列 size 做轻量近似统计，优先窃取较可能有任务的队列。

风险：低到中。需要确保不会降低任务最终可达性。

## 优先级 P2：指标热路径降噪

指标当前每个任务都会做 relaxed 原子写。虽然 relaxed 不建立同步关系，但在高频小任务下仍有缓存一致性成本。

可选优化：

- 每 worker 保存本地计数，`GetMetrics()` 时聚合。
- 对外部提交、拒绝、丢弃保留全局原子，对完成/失败/取消/窃取使用 worker-local 计数。
- 提供编译期开关关闭指标。

风险：中等。需要处理 CallerRuns 线程和外部线程的计数归属。

## 优先级 P2：等待与唤醒策略

当前每次成功入队后 `notify_one`，worker 空闲后等待条件变量。对于突发提交，频繁唤醒可能造成额外调度开销。

可选优化：

- 批量提交接口，一次入队多个任务后合并通知。
- 维护 sleeping worker 计数，只有确实存在睡眠 worker 时通知。
- worker 在短暂空闲时自旋几轮，再进入条件变量等待。

风险：中等。自旋会增加空闲 CPU 消耗，应通过 benchmark 控制。

## 优先级 P2：批量任务接口

大量极小任务是当前 benchmark 中最不利的场景。与其只优化调度器，可以提供批量接口让使用者表达更合适的任务粒度。

建议接口方向：

```cpp
SubmitRange(begin, end, chunkSize, function)
ParallelFor(first, last, grainSize, function)
```

收益：

- 显著减少 future 和任务包装数量。
- 降低队列操作次数。
- 更容易获得接近线性的 CPU 利用率。

风险：低到中。它可以作为新增 API，不影响现有语义。

## 优先级 P3：本地队列实现

`WorkStealingQueue` 当前使用 `std::deque + mutex`，易读可靠。若目标转向高性能运行时，可考虑 Chase-Lev work-stealing deque。

收益：

- owner push/pop 可减少锁竞争。
- thief steal 在高并发下更有优势。

风险：高。无锁 deque 的内存序、扩容、ABA 和关闭清理都更复杂，不适合作为第一阶段优化。

## API 与语义改进

除性能外，还有一些可维护性改进：

- 明确 `CallerRuns` 中内部 catch 只用于保护 worker/调用路径，future 仍保存用户异常。
- 为 `TaskHandle<void>` 增加专门测试，确认 `Get()` 语义。
- 为 `GetOptions()` 增加文档和测试。
- 为构造函数线程创建失败路径增加测试或故障注入。
- 考虑提供 `TrySubmit`，用返回值表达拒绝，减少异常作为控制流的成本。
- 考虑 `RequestStop()` 与 `Shutdown()` 分离，便于外部先停提交再自行等待。

## Benchmark 文件的直接改进点

`benchmarks/benchmark_runtime.cpp` 可以先做这些小步修改：

- [x] 修正注释中的 `namspace` 拼写。
- [x] 命令行支持多轮，例如 `--iterations 10`。
- [x] 输出 `RuntimeMetricsSnapshot`。
- [x] 把 `pool.Shutdown()` 的耗时单独记录。
- [x] 提供多线程 producer 场景。
- [ ] ~~提供 `CallerRuns` 和 `Reject` 策略场景。~~
- [x] 对错误参数打印 usage。

这些改动不会触碰核心运行时，适合作为第一批优化工作。

## 推荐路线

1. 先升级 benchmark，让结果可重复、可比较、可归档。
2. 增加批量任务或 `ParallelFor`，解决最常见的小任务吞吐问题。
3. 引入 move-only task wrapper，减少 `shared_ptr<packaged_task>` 成本。
4. 根据新 benchmark 结果决定是否拆分全局队列锁。
5. 最后再评估无锁 work-stealing deque。

这个顺序的核心原则是：先测量，再减少包装成本，然后优化竞争热点，最后才进入高复杂度的无锁结构。
