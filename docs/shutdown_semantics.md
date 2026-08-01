# MiniRuntime 关闭语义

## 1. 关闭接口

MiniRuntime 提供两个关闭接口：

```cpp
void Shutdown();
void ShutdownNow();
```

二者都会：

* 停止接收新任务；
* 唤醒 Block 提交线程；
* 等待已经开始运行的任务自然结束；
* 回收所有工作线程；
* 最终进入 `Stopped` 状态。

主要区别在于如何处理仍在队列中的等待任务。

---

## 2. 行为对比

| 行为          | `Shutdown()` | `ShutdownNow()`  |
| ----------- | ------------ | ---------------- |
| 接受新任务       | 否            | 否                |
| 已经运行的任务     | 继续执行         | 继续执行             |
| 全局等待任务      | 执行           | 丢弃               |
| 本地等待任务      | 执行           | 丢弃               |
| Block 提交线程  | 唤醒并拒绝        | 唤醒并拒绝            |
| Worker      | 排空任务后退出      | 当前任务结束后退出        |
| 丢弃任务 Future | 不适用          | `broken_promise` |
| 可重复调用       | 是            | 是                |
| 强制终止线程      | 否            | 否                |

---

## 3. `Shutdown()` 语义

`Shutdown()` 表示优雅关闭。

调用流程：

```text
获得 shutdownMutex
        ↓
state = Stopping
        ↓
停止接受新任务
        ↓
唤醒 Worker
        ↓
唤醒 Block 提交线程
        ↓
Worker 继续执行全部全局和本地任务
        ↓
pendingTasks == 0
        ↓
Worker 退出
        ↓
join 全部 Worker
        ↓
等待 CallerRuns 任务完成
        ↓
state = Stopped
```

`Shutdown()` 返回时保证：

* 所有成功入队任务均已执行；
* 所有 Worker 已退出；
* 所有 Worker 已被 `join()`；
* 没有等待任务；
* 状态为 `Stopped`。

---

## 4. `ShutdownNow()` 语义

`ShutdownNow()` 表示尽快关闭。

调用流程：

```text
获得 shutdownMutex
        ↓
state = Stopping
        ↓
discardPending = true
        ↓
停止接受新任务
        ↓
清空全局等待队列
        ↓
清空所有本地等待队列
        ↓
更新 discarded 指标
        ↓
销毁被丢弃的 packaged_task
        ↓
唤醒 Worker 和 Block 提交线程
        ↓
已运行任务自然结束
        ↓
Worker 退出
        ↓
join 全部 Worker
        ↓
等待 CallerRuns 任务完成
        ↓
state = Stopped
```

---

## 5. `ShutdownNow()` 不强制终止任务

`ShutdownNow()` 只丢弃尚未开始的任务。

已经开始运行的任务不会被强制中断。

原因是 C++ 不提供安全、通用的强制线程终止机制。

任务可能正在：

* 持有互斥锁；
* 修改共享数据；
* 更新多个关联字段；
* 分配或释放资源；
* 写文件；
* 执行事务。

强制终止可能造成：

* 锁永远无法释放；
* 数据结构损坏；
* 资源泄漏；
* 程序状态不一致。

需要终止正在运行的任务时，应使用协作式取消。

---

## 6. 被丢弃任务的 Future

等待队列中的任务通常由：

```cpp
std::packaged_task
```

包装。

`ShutdownNow()` 清空队列后，未执行的 `packaged_task` 被销毁。

对应共享状态进入：

```text
broken promise
```

调用：

```cpp
future.get();
```

会抛出：

```cpp
std::future_error
```

错误码：

```cpp
std::future_errc::broken_promise
```

这与协作式取消不同：

```text
TaskCancelled   → 任务执行包装器并观察到取消
broken_promise  → 任务未开始就被运行时直接丢弃
```

---

## 7. 新任务拒绝

从状态变为：

```cpp
RuntimeState::Stopping
```

开始，新的 `Submit()` 和 `SubmitCancelable()` 会抛出：

```cpp
TaskRejected
```

以下场景都属于任务拒绝：

* 线程池正在关闭；
* 线程池已经停止；
* Reject 策略下全局队列已满；
* Block 提交者等待期间线程池开始关闭。

---

## 8. Block 提交线程

Block 策略下，提交线程可能等待：

```text
全局队列出现空间
```

关闭线程池时必须：

```cpp
queueNotFullCv.notify_all();
```

提交线程醒来后重新检查状态。

如果状态不再是 `Running`，抛出：

```cpp
TaskRejected
```

否则，关闭线程可能永久等待仍被阻塞的提交线程。

---

## 9. 多线程并发关闭

多个线程可能同时调用：

```cpp
Shutdown();
ShutdownNow();
```

`shutdownMutex` 保证只有一个线程执行：

* 状态切换；
* 队列清理；
* Worker 通知；
* Worker `join()`；
* 最终状态设置。

后续关闭调用获得锁后，如果状态已经是：

```cpp
Stopped
```

则直接返回。

---

## 10. 混合关闭模式

当前实现将完整关闭流程串行化。

第一个真正进入关闭流程的调用决定实际关闭行为。

例如：

```text
Thread A: Shutdown()
Thread B: ShutdownNow()
```

如果 `Shutdown()` 先获得 `shutdownMutex`，线程池将执行优雅排空。

如果 `ShutdownNow()` 先获得锁，等待任务将被丢弃。

因此，不建议业务代码在多个位置混合调用不同关闭模式。

应由统一生命周期管理者决定关闭策略。

---

## 11. 任务内部关闭

当前实现禁止任务内部调用所属线程池的关闭接口。

例如：

```cpp
pool.Submit([&pool] {
    pool.Shutdown();
});
```

会抛出：

```cpp
std::logic_error
```

原因：

* Worker 不能 `join()` 自己；
* CallerRuns 任务可能等待自身计数归零；
* 任务内部关闭会让生命周期责任变得不清晰。

线程池关闭应由外部生命周期管理线程执行。

---

## 12. 析构函数

析构函数调用：

```cpp
Shutdown();
```

因此默认采用优雅关闭语义。

对象离开作用域时：

* 停止接受任务；
* 执行全部等待任务；
* 等待 Worker 退出；
* 回收线程资源。

析构函数不得抛出异常。

调用方应确保：

* 不从 Worker 内部销毁所属线程池；
* 没有其他线程继续访问即将析构的线程池对象。

---

## 13. 关闭相关锁规则

关闭流程中只能短暂持有运行时主锁，用于：

* 检查状态；
* 设置 `Stopping`；
* 清空全局队列；
* 更新受保护状态。

执行以下操作时不能持有运行时主锁：

* 销毁大量任务；
* 等待用户任务；
* `join()` Worker；
* 调用用户代码。

`shutdownMutex` 可以在整个关闭流程中持有，因为 Worker 不会获取它。

---

## 14. 关闭正确性不变量

### 优雅关闭

```text
submitted
=
completed + failed + cancelled
```

优雅关闭不会产生 `discarded`。

### 立即关闭

```text
submitted
=
completed + failed + cancelled + discarded
```

在并发观测瞬间，指标可能尚未完全收敛；关闭完成后应满足该关系。

### 最终状态

关闭完成后：

```text
PendingTaskCount() == 0
activeTasks == 0
state == Stopped
所有 Worker 不再 joinable
```

---

## 15. 测试场景

关闭语义应覆盖：

* 无任务关闭；
* 重复关闭；
* 析构自动关闭；
* 关闭后提交；
* 多线程并发关闭；
* 优雅排空全局任务；
* 优雅排空本地任务；
* 立即丢弃全局任务；
* 立即丢弃本地任务；
* 已运行任务不被强制终止；
* Block 提交者在关闭时被唤醒；
* CallerRuns 任务结束后关闭才能返回；
* 被丢弃任务 Future 抛出 `broken_promise`。
