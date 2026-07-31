# Day 3：背压、拒绝策略与协作式取消

## 今日目标

在 Day 2 泛型异步线程池的基础上，增加生产环境中非常重要的过载保护与任务控制能力：

- 将无限任务队列改为有界任务队列
- 实现 3 种任务拒绝策略：`Block`、`Reject`、`CallerRuns`
- 保证阻塞提交线程在线程池关闭时能够退出
- 支持任务开始前取消、运行中协作式取消：`CancellationToken`、`TaskHandle<T>`
- 支持超时等待
- 增加运行指标，支持统计 `ShutdownNow()` 丢弃的任务数量

## 为什么需要有界队列

Day 2 的任务队列没有容量限制，如果任务提交速度持续高于任务执行速度，队列会不断增长，就有可能耗尽内存。

无限队列只是把过载问题推迟到了内存耗尽时。因此，有界队列通过 `queueCapacity` 限制仍在等待执行的任务数量。当队列满时，线程池必须明确决定如何处理新任务。

## 三种拒绝策略

### Block

队列已满时，阻塞任务提交线程：

```
队列已满
   ↓
提交线程等待
   ↓
工作线程取走任务
   ↓
队列出现空间
   ↓
提交线程继续入队
```

**优点：**

- 不会直接丢失任务；
- 能自然降低生产速度；
- 可以形成背压；

**缺点：**

- 提交线程可能长时间阻塞；
- 工作线程递归提交任务时可能死锁；
- 请求处理线程被阻塞后，系统响应时间可能增加

### Reject

队列已满时立即抛出：`minirt::TaskRejected`。

**优点：**

- 快速失败；
- 调用者可以自行决定重试、降级或返回错误；
- 不会长时间阻塞调用线程；

**缺点：**

- 调用者必须正确处理拒绝；
- 如果上层无脑重试，可能进一步加重系统压力。

### CallerRuns

队列已满时，由提交任务的线程执行任务。

**优点：**

- 不丢任务；
- 不需要等待队列空间；
- 提交线程执行任务后，后续提交速度自然降低。

**缺点：**

- 用户任务会占用提交线程；
- 任务执行线程不再固定为工作线程；
- 如果调用方依赖线程局部变量，需要明确其语义；
- 长任务可能显著阻塞提交者；

## 两个条件变量

当前线程池使用两个条件变量：

1. 工作线程等待：`m_taskAvailableCv`。条件为：队列中有任务或者线程池开始关闭。
2. Block 策略下的提交线程等待：`m_queueNotFullCv`。条件为：队列出现空间或者线程池开始关闭

通知原则：

1. 工作线程从队列取出任务后调用：`m_queueNotFullCv.notify_one();`
2. 线程池关闭时同时调用：`m_taskAvailableCv.notify_all()`、`m_queueNotFullCv.notify_all()`。否则阻塞在等待队列空间的提交线程可能永远无法退出。

## Block 策略的递归提交死锁

考虑一个工作线程的线程池：

```
工作线程正在执行 Task A

等待队列已经达到容量上限

Task A 内部调用 Submit(Task B)
```

如果 Submit() 使用 Block 策略：

```
Task A 等待队列出现空间

队列空间需要工作线程取走任务

唯一工作线程仍在执行 Task A
```

此时形成死锁。

当前实现使用：

```cpp
static thread_local ThreadPool* current_pool_;
```

识别当前线程是否正在执行本线程池任务。

当满足：

```cpp
m_currentPool == this && m_globalTasks.size() >= queueCapacity
```

时，Block 策略退化为当前线程直接执行，避免唯一工作线程阻塞等待自身释放队列空间。

该处理只能解决“队列已满时的直接递归提交”问题。任务之间相互等待 future，仍然可能形成更复杂的线程池饥饿或死锁。


## m_activeTasks

任务从队列取出后，不再属于 `m_globalTasks`，但它仍然没有完成。

因此线程池增加：`std::size_t m_activeTasks` 统计已经开始运行但尚未结束的任务。

包括：

- 工作线程执行的任务；
- CallerRuns 策略下由提交线程执行的任务；
- Block 策略为避免递归死锁而内联执行的任务。

任务开始前：++m_activeTasks;

任务完成后：--m_activeTasks;

关闭线程池时，不仅需要等待工作线程退出，还需要等待 CallerRuns 任务结束。

## 协作式取消

C++ 无法安全地从外部强制终止一个正在执行的线程，因此本项目使用协作式取消。

1. 调用者请求取消：handle.Cancel();
2. 任务主动检查：token.ThrowIfCancellationRequested();

完整示例：

```cpp
auto handle = pool.SubmitCancelable([](minirt::CancellationToken token) {
    while (true) {
        token.ThrowIfCancellationRequested();
        ProcessOneChunk();
    }
});

handle.Cancel();
```

## CancellationState

`TaskHandle` 和 `CancellationToken` 共享：

```
std::shared_ptr<CancellationState>
```

其中保存：

```
std::atomic<bool> requested;
```

关系如下：

```
TaskHandle
    │
    ├── Cancel()
    │      ↓
    │  requested = true
    │
共享 CancellationState
    │
    └── CancellationToken
             ↓
       IsCancellationRequested()
```

TaskHandle 负责写入取消请求，CancellationToken 负责读取取消状态。


## 运行指标

当前指标包括：

- submitted
- completed
- failed
- cancelled
- rejected
- discarded
- callerRuns

### submitted

成功被线程池接受的任务数量。

包含：

- 入队任务；
- CallerRuns 任务；
- Block 策略递归提交时的内联任务。

不包含提交阶段被拒绝的任务。

### completed

用户函数正常返回的任务数量。

### failed

用户函数以普通异常结束的任务数量。

### cancelled

任务以 TaskCancelled 异常结束的数量。

只调用了 Cancel()，但任务没有响应取消，不会计入。

### rejected

在提交阶段因为线程池停止或队列已满而被拒绝的任务数量。

### discarded

被 ShutdownNow() 从等待队列中移除的任务数量。

### callerRuns

由提交线程直接执行的任务数量。

也包含 Block 策略为避免工作线程递归提交死锁而内联执行的任务。

## 今日测试覆盖

### 背压测试
- [x] Reject 策略在队列满时抛出异常；
- [x] CallerRuns 在提交线程中执行；
- [x] Block 策略等待队列空间；
- [x] 线程池关闭能够唤醒阻塞提交者；
- [x] 工作线程递归提交不会因为满队列而死锁。

### 取消测试
- [x] 任务开始前取消；
- [x] 运行中协作式取消；
- [x] 不检查 token 的任务不会被强制停止；
- [x] WaitFor() 超时不会自动取消；
- [x] 支持 void 可取消任务；
- [x] 多次调用 Cancel() 是安全的。

### 指标测试
- [x] 正常完成计数；
- [x] 异常任务计数；
- [x] 取消任务计数；
- [x] 拒绝任务计数；
- [x] CallerRuns 计数；
- [x] ShutdownNow 丢弃任务计数。

## 当前限制

当前仍存在以下限制：

- 所有工作线程共享一个全局任务队列；
- 全局 mutex 仍然可能成为竞争点；
- 没有每线程本地队列；
- 没有工作窃取；
- 不支持任务优先级；
- 不支持定时任务；
- 不支持任务依赖；
- 不支持 C++20 stop_token；
- 不支持从任务内部关闭或销毁所属线程池；
- 任务互相等待 future 时仍可能产生线程池饥饿；
- 指标没有延迟分布和队列高水位；
- 尚未通过 ThreadSanitizer 检查。

下一阶段将重点解决：

```
单全局队列竞争
    ↓
每线程本地队列
    ↓
工作窃取
    ↓
负载不均衡
```

## 今日验收标准

- [x] 无限任务队列为什么危险；
- [x] 背压的含义；
- [x] Block、Reject 和 CallerRuns 的区别；
- [x] Block 策略为什么可能死锁；
- [x] 为什么需要两个条件变量；
- [x] 为什么线程池关闭时要唤醒阻塞提交者；
- [x] CallerRuns 为什么能降低提交速度；
- [x] 为什么不能强制终止 C++ 线程；
- [x] 协作式取消如何工作；
- [x] 请求取消与任务已经取消有什么区别；
- [x] 超时等待为什么不会自动停止任务；
- [x] TaskHandle 和 CancellationToken 如何共享状态；
- [x] cancelled 与 discarded 有什么区别；
- [x] 为什么指标可以使用 relaxed 原子操作；
- [x] m_activeTasks 为什么不能只通过队列大小代替。