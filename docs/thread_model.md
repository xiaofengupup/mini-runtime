# MiniRuntime 线程模型

本文说明 MiniRuntime 中的线程角色、锁职责、worker 身份识别和任务执行规则。

## 线程角色

运行时涉及三类执行上下文：

- 外部提交线程：不属于当前线程池的调用线程。
- worker 线程：线程池构造时创建的固定工作线程。
- CallerRuns 线程：因背压策略或单 worker 内部提交而直接执行任务的提交线程。

## 外部提交线程

外部线程调用 `Submit` 或 `SubmitCancelable` 时，任务进入有界全局队列。提交行为受以下配置影响：

```cpp
ThreadPoolOptions::queueCapacity
ThreadPoolOptions::rejectionPolicy
```

外部队列满时：

- `Block` 等待空间。
- `Reject` 抛出 `TaskRejected`。
- `CallerRuns` 由提交线程直接执行任务。

## Worker 线程

`ThreadPool` 构造时按 `threadCount` 创建固定数量 worker。每个 worker 运行：

```cpp
WorkerLoop(workerIndex);
```

worker 的主循环：

```text
set thread-local identity
while true:
  if discardPending: exit
  try local queue
  try global queue
  try steal from other workers
  if got task:
    activeTasks++
    run task outside locks
    activeTasks--
    continue
  wait for task or shutdown
  if stopping and no pending task: exit
clear thread-local identity
```

## Worker 身份

当前实现使用 thread-local 变量识别当前线程是否属于某个线程池：

```cpp
static thread_local ThreadPool* m_currentPool;
static thread_local std::size_t m_currentWorkerIndex;
```

worker 启动时设置身份，退出前清除身份。该机制用于：

- 判断提交是否来自当前线程池的 worker。
- 将 worker 内部提交放入对应本地队列。
- 在单 worker 场景内联执行子任务，避免自我等待死锁。
- 禁止任务内部调用所属线程池的关闭接口。
- 在 CallerRuns 执行期间标记当前线程正在执行本线程池任务。

CallerRuns 执行时，`m_currentPool` 临时设置为当前线程池，`m_currentWorkerIndex` 设置为 `m_kNoWorker`。

## 全局队列

全局队列保存外部提交的等待任务：

```cpp
std::queue<Task> m_globalTasks;
```

它由 `m_mutex` 保护，并通过 `queueCapacity` 控制容量。worker 从全局队列取出任务后，会通知一个等待队列空间的 Block 提交线程。

## 本地队列

每个 worker 拥有一个独立本地队列：

```cpp
std::vector<std::unique_ptr<WorkStealingQueue<Task>>> m_localQueues;
```

本地队列内部有自己的 mutex。worker 内部提交任务时，如果线程池有多个 worker，任务会进入当前 worker 的本地队列。

本地队列操作：

```text
owner Push      -> back
owner TryPop    -> back
thief TrySteal  -> front
```

运行时不会在 worker 启动后增删本地队列，因此 `m_localQueues` 中的队列对象地址保持稳定。

## 锁职责

`m_mutex` 保护：

- `m_globalTasks`
- `m_state`
- `m_activeTasks`

`m_mutex` 不保护：

- 本地队列内部 `deque`
- `m_pendingTasks`
- `m_discardPending`
- 原子指标

`WorkStealingQueue` 内部 mutex 只保护对应本地队列的 `deque`。窃取时一次只访问一个 victim 队列。

`m_shutdownMutex` 串行化：

- `Shutdown()`
- `ShutdownNow()`
- 析构函数中的 `Shutdown()`

worker 不获取 `m_shutdownMutex`。

## 条件变量

`m_taskAvailableCv` 用于 worker 等待：

```text
pendingTasks > 0
or state != Running
```

`m_queueNotFullCv` 用于 Block 提交者等待：

```text
globalTasks.size() < queueCapacity
or state != Running
```

`m_idleCv` 用于关闭线程等待：

```text
activeTasks == 0
```

条件变量等待都必须通过谓词重新检查条件，以处理虚假唤醒。

## 执行锁规则

用户任务必须在运行时内部锁之外执行。

```text
lock
  pop task and update counters
unlock
run user task
lock
  finish task and notify idle waiters
unlock
```

这样可以避免用户任务阻塞提交、阻塞其他 worker 取任务，或在递归提交时造成锁重入问题。

## Join 规则

关闭流程不能在持有 `m_mutex` 时调用 `worker.join()`。

正确顺序：

```text
lock m_mutex
  set Stopping
unlock
notify workers and submitters
join workers
lock m_mutex
  wait activeTasks == 0
  set Stopped
unlock
```

如果持锁 join，worker 退出路径可能也需要同一把锁，容易形成死锁。

## 单 Worker 内部提交

单 worker 线程池中，任务内部提交子任务并等待子任务 future 是常见死锁形态：

```text
only worker runs parent
parent submits child
child waits in queue
parent waits child future
no worker remains to run child
```

当前实现检测到这种情况时直接在当前线程执行子任务，并计入 `callerRuns`。

## 工作窃取边界

工作窃取可以改善：

- worker 之间负载不均。
- 子任务集中在某个 worker 本地队列。
- 部分任务执行时间明显长于其他任务。

它不能解决：

- 用户任务之间的循环等待。
- 所有 worker 同时等待尚未执行的子任务。
- 用户代码的锁顺序错误。
- 阻塞式 I/O 长时间占满 worker。
