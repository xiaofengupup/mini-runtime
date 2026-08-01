# MiniRuntime 线程模型

## 1. 线程角色

MiniRuntime 中存在三类执行线程：

```text
外部提交线程
工作线程
CallerRuns 执行线程
```

---

## 2. 外部提交线程

外部提交线程通常是：

* 主线程；
* 业务请求线程；
* 测试线程；
* 其他不属于当前线程池的线程。

外部线程调用：

```cpp
pool.Submit(task);
```

任务进入全局有界队列。

外部提交受以下配置约束：

```text
queueCapacity
RejectionPolicy
```

---

## 3. 工作线程

线程池构造时创建固定数量的 Worker：

```cpp
std::vector<std::thread> workers;
```

每个 Worker 具有唯一索引：

```text
0
1
2
...
threadCount - 1
```

线程入口：

```cpp
WorkerLoop(workerIndex);
```

每个 Worker 拥有一个本地任务队列：

```text
localQueues[workerIndex]
```

Worker 获取任务的顺序为：

```text
Local queue
    ↓
Global queue
    ↓
Steal from another local queue
    ↓
Condition-variable wait
```

---

## 4. Worker 身份

运行时使用：

```cpp
static thread_local ThreadPool* currentPool;
static thread_local std::size_t currentWorkerIndex;
```

识别当前线程是否属于某个线程池。

Worker 启动时：

```cpp
currentPool = this;
currentWorkerIndex = workerIndex;
```

Worker 退出前：

```cpp
currentWorkerIndex = kNoWorker;
currentPool = nullptr;
```

该信息用于：

* 判断提交是否来自 Worker；
* 将内部提交任务放入正确本地队列；
* 避免单 Worker 内部提交后等待造成死锁；
* 禁止任务内部关闭所属线程池；
* 识别 CallerRuns 上下文。

---

## 5. CallerRuns 执行线程

当全局队列已满且策略为：

```cpp
RejectionPolicy::CallerRuns
```

任务由提交线程直接执行。

CallerRuns 任务：

* 不进入全局队列；
* 不进入本地队列；
* 计入 `submitted`；
* 计入 `callerRuns`；
* 计入 `activeTasks`；
* 执行完成后减少 `activeTasks`。

CallerRuns 是一种背压机制，因为提交线程执行任务期间无法继续高速提交。

---

## 6. 全局任务队列

全局队列：

```cpp
std::queue<Task> globalTasks;
```

用途：

* 保存外部线程提交的任务；
* 提供有界容量；
* 实现 Block、Reject 和 CallerRuns。

受运行时主锁保护：

```cpp
std::mutex mutex;
```

全局队列容量只限制等待中的外部任务，不包含：

* 正在执行的任务；
* 本地队列任务；
* CallerRuns 任务。

---

## 7. 本地任务队列

每个 Worker 拥有独立的：

```cpp
WorkStealingQueue<Task>
```

本地队列内部有自己的互斥锁：

```cpp
std::mutex mutex;
std::deque<Task> queue;
```

Owner 操作：

```text
Push      → back
TryPop    → back
```

Thief 操作：

```text
TrySteal  → front
```

本地队列不使用运行时主锁保护。

运行时运行期间不会增加、删除或重新分配 `localQueues` 容器中的元素。

---

## 8. 锁职责

### 8.1 运行时主锁

```cpp
std::mutex mutex;
```

保护：

* `globalTasks`；
* `state`；
* `activeTasks`。

它不保护：

* 本地队列内部 deque；
* `pendingTasks`；
* `discardPending`；
* 原子指标。

---

### 8.2 本地队列锁

每个 `WorkStealingQueue` 内部拥有一把锁。

只保护该队列的：

```cpp
std::deque<Task>
```

Worker 窃取任务时，一次只锁一个本地队列。

不能同时持有多个本地队列锁，否则容易引入锁顺序问题。

---

### 8.3 关闭锁

```cpp
std::mutex shutdownMutex;
```

用于串行化：

```text
Shutdown
ShutdownNow
析构函数中的 Shutdown
```

只有一个线程可以执行完整关闭流程和 `join()`。

Worker 不会获取 `shutdownMutex`。

---

## 9. 原子变量

### `pendingTasks`

```cpp
std::atomic<std::size_t> pendingTasks;
```

表示所有等待队列中的任务总数。

任务进入全局或本地队列时加一。

任务从全局或本地队列取出时减一。

任务被 `ShutdownNow()` 丢弃时减去丢弃数量。

---

### `discardPending`

```cpp
std::atomic<bool> discardPending;
```

表示是否进入立即关闭模式。

Worker 观察到该标志后停止获取新任务。

---

### Metrics

运行指标使用原子计数器。

指标使用 `memory_order_relaxed`，因为只要求计数正确，不承担同步用户数据的职责。

---

## 10. 条件变量

### `taskAvailableCv`

等待者：

```text
Worker
```

唤醒条件：

```text
有新任务
或者
线程池状态发生变化
```

---

### `queueNotFullCv`

等待者：

```text
Block 策略下的外部提交线程
```

唤醒条件：

```text
全局队列出现空间
或者
线程池开始关闭
```

---

### `idleCv`

等待者：

```text
关闭线程
```

条件：

```text
activeTasks == 0
```

---

## 11. Worker 主循环

```text
设置 thread_local Worker 身份
            ↓
检查 discardPending
            ↓
TryPopLocal
            ↓
TryPopGlobal
            ↓
TrySteal
            ↓
找到任务？
  ├── 是 → activeTasks++ → 执行 → activeTasks--
  └── 否 → 等待 taskAvailableCv
            ↓
检查关闭条件
            ↓
退出循环
            ↓
清除 thread_local Worker 身份
```

---

## 12. 任务执行锁规则

运行时只在锁内完成：

* 检查状态；
* 入队；
* 出队；
* 更新内部计数；
* 检查等待条件。

用户任务必须在所有运行时内部锁之外执行：

```cpp
Task task;

{
    // Lock.
    task = PopTask();
}

// Unlock.

task();
```

原因：

* 用户任务执行时间不可控；
* 用户任务可能阻塞；
* 用户任务可能再次提交任务；
* 其他 Worker 需要继续获取任务；
* 提交线程需要继续入队。

---

## 13. `join()` 锁规则

严禁在持有运行时主锁时调用：

```cpp
worker.join();
```

错误关系：

```text
Shutdown thread:
holds mutex
waits for worker.join()

Worker:
waits for mutex
before exiting
```

形成死锁。

正确流程：

```text
锁内设置 Stopping
释放主锁
notify_all
join workers
重新加锁设置 Stopped
```

---

## 14. 单 Worker 内部提交

假设线程池只有一个 Worker：

```text
Worker executing parent
    ↓
parent submits child
    ↓
child enters local queue
    ↓
parent waits child future
```

唯一 Worker 正在等待，因此没有线程可以执行 child。

为避免该问题，单 Worker 场景下的内部提交直接在当前线程执行。

---

## 15. 工作窃取的边界

工作窃取可以改善：

* Worker 之间负载不均；
* 嵌套任务集中在单个本地队列；
* 部分任务执行时间差异较大。

工作窃取不能解决：

* 循环依赖；
* 所有 Worker 同时等待未执行子任务；
* 用户锁顺序错误；
* 任务之间的逻辑死锁。

---

## 16. 线程生命周期

```text
ThreadPool constructor
    ↓
Create all local queues
    ↓
Set state to Running
    ↓
Start workers
    ↓
Run tasks
    ↓
Shutdown / ShutdownNow
    ↓
Set state to Stopping
    ↓
Wake workers
    ↓
Workers exit
    ↓
Join all workers
    ↓
Set state to Stopped
    ↓
Destroy ThreadPool members
```

必须先创建全部本地队列，再启动 Worker。

必须先 `join()` 全部 Worker，再销毁：

* mutex；
* condition_variable；
* localQueues；
* globalTasks；
* metrics。
