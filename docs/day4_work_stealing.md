# Day 4：每线程本地队列与工作窃取

## 1. 今日目标

在 Day 3 单全局任务队列基础上增加：

- 每工作线程本地双端队列；
- 工作线程内部提交进入本地队列；
- 外部线程提交继续进入全局有界队列；
- 本地 LIFO 执行；
- 跨线程 FIFO 窃取；
- 原子待执行任务计数；
- Shutdown 排空所有本地队列；
- ShutdownNow 丢弃所有队列中的等待任务；
- 工作窃取运行指标；
- 工作窃取单元测试和集成测试。

## 2. 调度模型

外部线程提交：

    external thread
          ↓
    global bounded queue

工作线程内部提交：

    worker N
       ↓
    local queue N

工作线程获取任务顺序：

    local queue
        ↓
    global queue
        ↓
    steal from other workers
        ↓
    condition variable wait

## 3. 本地队列方向

本地线程从队尾取任务：

    push_back
    pop_back

其他线程从队头窃取：

    pop_front

本地线程使用 LIFO，窃取线程使用 FIFO。

## 4. 为什么引入本地队列

单全局队列要求所有工作线程竞争同一把 mutex。

本地队列使工作线程在大部分情况下只操作自己的队列。只有本地没有任务时，才访问全局队列或其他线程的队列。

## 5. 外部提交与内部提交

外部提交继续使用全局有界队列，因此仍受以下策略控制：

- Block
- Reject
- CallerRuns

工作线程内部提交进入本地队列。本地队列当前没有容量限制，这是 Day 4 的已知限制。

## 6. 单工作线程特殊语义

如果只有一个工作线程，内部任务不能简单进入本地队列。

父任务可能提交子任务后立即等待子任务 future，而唯一工作线程仍在执行父任务，导致死锁。

因此单工作线程的内部提交直接在当前线程执行。

## 7. pending_tasks_

pending_tasks_ 统计：

- 全局队列任务；
- 所有本地队列任务。

不统计已经被 Worker 取出的 active_tasks_。

任务入队时加一，任务从任意队列取出或被 ShutdownNow 丢弃时减一。

## 8. Shutdown

Shutdown 设置状态为 Stopping，但不会设置 discard_pending。

Worker 会继续按照本地、全局、窃取的顺序处理剩余任务。

当 pending_tasks_ 等于 0 时，工作线程退出。

## 9. ShutdownNow

ShutdownNow：

1. 将状态改为 Stopping；
2. 设置 discard_pending；
3. 清空全局队列；
4. 清空所有本地队列；
5. 销毁未执行的 packaged_task；
6. 唤醒所有工作线程和阻塞提交线程；
7. 等待已经开始运行的任务自然结束；
8. 将状态改为 Stopped。

已经被工作线程取出的任务仍然允许执行。

## 10. 工作窃取指标

local_submitted 表示提交到本地队列的任务数量。

stolen 表示其他 Worker 成功从某个本地队列窃取的任务数量。

指标使用 relaxed 原子操作，只用于统计，不用于任务完成同步。

## 11. 当前限制

- 本地队列不是无锁队列；
- 本地队列没有容量限制；
- 窃取顺序采用固定轮询；
- 没有随机选择 victim；
- 没有批量窃取；
- 没有任务优先级；
- 没有 NUMA 感知；
- 没有公平性调度；
- 本地任务持续生成时，全局任务可能等待更久；
- 工作窃取不能解决循环依赖和所有 future 饥饿问题。

## 12. 今日掌握内容

- 全局队列锁竞争；
- 每线程本地队列；
- deque 双端操作；
- 本地 LIFO；
- 窃取 FIFO；
- thread_local Worker 身份；
- 原子待执行计数；
- Shutdown 对多个队列的处理；
- ShutdownNow 与并发取任务的边界；
- 工作窃取适用场景和限制。