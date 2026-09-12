# MiniRuntime 关闭语义

MiniRuntime 提供两个关闭接口：

```cpp
void Shutdown();
void ShutdownNow();
```

二者都会停止接受新任务、唤醒等待者、等待已开始运行的任务自然结束，并最终 join 所有 worker。差异在于等待队列中的任务是否继续执行。

## 行为对比

| 行为 | `Shutdown()` | `ShutdownNow()` |
| --- | --- | --- |
| 接受新任务 | 否 | 否 |
| 已经运行的任务 | 继续运行 | 继续运行 |
| 全局等待任务 | 继续执行 | 丢弃 |
| 本地等待任务 | 继续执行 | 丢弃 |
| Block 提交线程 | 唤醒后拒绝 | 唤醒后拒绝 |
| worker 退出 | 排空任务后退出 | 当前任务结束后退出 |
| 被丢弃任务 future | 不适用 | `broken_promise` |
| 可重复调用 | 是 | 是 |
| 强制终止线程 | 否 | 否 |

## 优雅关闭

`Shutdown()` 的目标是排空已经接受的任务。

```text
Shutdown()
  -> acquire shutdownMutex
  -> state = Stopping
  -> notify workers
  -> notify Block submitters
  -> workers drain global and local queues
  -> pendingTasks becomes 0
  -> workers exit
  -> join workers
  -> wait activeTasks == 0
  -> state = Stopped
```

返回时保证：

- 不再接受新任务。
- 已成功入队的任务都已执行。
- worker 已退出并被 join。
- `PendingTaskCount() == 0`。
- 状态为 `RuntimeState::Stopped`。

## 立即关闭

`ShutdownNow()` 的目标是尽快停止等待任务的执行。

```text
ShutdownNow()
  -> acquire shutdownMutex
  -> state = Stopping
  -> move global waiting tasks to local discarded list
  -> drain local queues to local discarded list
  -> pendingTasks -= discarded count
  -> metrics.discarded += discarded count
  -> notify workers
  -> notify Block submitters
  -> destroy discarded tasks outside m_mutex
  -> already-running tasks finish naturally
  -> join workers
  -> wait activeTasks == 0
  -> state = Stopped
```

`ShutdownNow()` 只丢弃尚未开始执行的任务。已经从队列取出并开始运行的任务不会被强制取消。

## Future 行为

正常执行的任务由 `std::packaged_task` 写入结果或异常：

```text
task returns value -> future.get() returns value
task throws        -> future.get() rethrows
task cancels       -> future.get() throws TaskCancelled
```

`ShutdownNow()` 丢弃的等待任务没有机会执行 packaged task。对应 future 在 `get()` 时抛出：

```cpp
std::future_error
```

错误码为：

```cpp
std::future_errc::broken_promise
```

这与协作式取消不同：

```text
TaskCancelled  : task started and observed cancellation
broken_promise : task never started and was discarded
```

## 新任务拒绝

线程池进入 `Stopping` 后，`Submit` 和 `SubmitCancelable` 都会抛出 `TaskRejected`。

会产生拒绝的场景：

- 线程池正在关闭。
- 线程池已经停止。
- `Reject` 策略下全局队列已满。
- `Block` 策略等待队列空间时线程池开始关闭。

被拒绝任务不计入 `submitted`，但计入 `rejected`。

## Block 提交者

`Block` 策略下，提交者可能正在等待全局队列空间。关闭开始时必须通知：

```cpp
m_queueNotFullCv.notify_all();
```

提交者醒来后重新检查状态。如果状态不再是 `Running`，会抛出 `TaskRejected`。

## 并发关闭

多个线程可以同时调用关闭接口。`m_shutdownMutex` 保证完整关闭流程串行执行。第一个获得锁并进入关闭流程的调用决定实际关闭模式。

```text
Thread A: Shutdown()
Thread B: ShutdownNow()
```

如果 A 先获得锁，将执行优雅排空。如果 B 先获得锁，将丢弃等待任务。业务代码应尽量由统一生命周期管理者决定关闭策略。

## 任务内部关闭

当前实现禁止任务内部关闭所属线程池：

```cpp
pool.Submit([&pool] {
    pool.Shutdown();
});
```

这种调用会抛出 `std::logic_error`。原因包括：

- worker 不能 join 自己。
- CallerRuns 任务可能等待自身 active 计数归零。
- 生命周期所有权会变得模糊。

关闭应由线程池外部的生命周期管理线程触发。

## 析构函数

析构函数调用 `Shutdown()`，因此默认采用优雅关闭。对象离开作用域时会排空已经接受的任务并回收 worker。

调用方仍需保证：

- 不从当前线程池任务内部销毁所属线程池对象。
- 没有其他线程在对象析构后继续访问它。

## 关闭不变量

优雅关闭完成后：

```text
submitted = completed + failed + cancelled
discarded = 0
PendingTaskCount() = 0
state = Stopped
```

立即关闭完成后：

```text
submitted = completed + failed + cancelled + discarded
PendingTaskCount() = 0
state = Stopped
```

并发观测时指标可能暂未收敛，应以关闭完成后的快照作为最终结果。
