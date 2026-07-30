# 泛型任务与线程池生命周期

## 今日目标

在 Day 1 基础线程池的基础上，完成以下升级：

1. 支持提交任意可调用对象；
2. 支持向任务传递参数；
3. 支持任务返回值；
4. 支持 `void` 返回类型；
5. 支持 `move-only` 参数；
6. 使用 `std::future` 获取异步结果；
7. 使用 `std::packaged_task` 传播任务异常；
8. 使用状态机管理线程池生命周期；
9. 实现优雅关闭 `Shutdown`；
10. 实现立即关闭 `ShutdownNow`；
11. 支持多个线程并发调用关闭接口；
12. 使用 GoogleTest 验证任务提交和生命周期语义；

## 接口变化

### 提交任务

Day 1 的任务接口只能接收已经完成类型擦除的 void() 任务：

```cpp
void Submit(std::function<void()> task);
void Stop();
```

Day 2 将 `Submit()` 修改为**函数模板（变参模板）**：

```cpp
template<typename F, typename... Args>
auto Submit(F&& f, Args&... args);
```

调用方式：

```cpp
minirt::ThreadPool pool(2);

auto future = pool.Submit([](int l, int r){
    return l + r;
}, 10, 20);

const int result = future.get();
```

### 关闭线程池

线程池关闭接口调整为：

```cpp
void Shutdown();
void ShutdownNow();
```

删除 `Stop()` 接口；

### 状态查询

删除 `m_stopping`，增加 `m_state` 和状态查询接口：

```cpp
RuntimeState GetState() const;
```

## 泛型任务提交

### 为什么 Submit 是函数模板

不同任务具有不同的：

- 函数类型；
- 参数类型；
- 参数数量；
- 返回值类型。

每个 lambda 都有独立且匿名的类型，因此需要通过模板接收：

```cpp
template <typename F, typename... Args>
auto Submit(F&& function, Args&&... args);
```

其中：

- F 表示可调用对象类型；
- Args... 表示可变数量的参数类型；
- F&& 和 Args&&... 是转发引用；
- std::forward 调用参数的左值或右值属性。

### 为什么模板定义放在头文件

模板不是普通函数，编译器只有在看到具体调用时，才会根据实际生成对应代码。例如：

```cpp
pool.Submit([] {
    return 1;
});
```

编译器会为这个 `lambda` 的具体类型实例化一个 `Submit()`。

如果模板只有声明出现在头文件中，而定义放在某个 cpp 文件中，那么**调用代码所在编译单元也就无法看到完整定义**，也就无法完成模板实例化。

因此，`Submit()` 的完整定义需要放在 `thread_pool.h` 中。

## 参数存储与生命周期

异步任务通常不会在 `Submit()` 期间立即调用执行，`Submit()` 返回后，任务还可能继续留在任务队列中。因此，任务不能简单保存 `function` 和 `args` 的转发引用，因为这些引用可能在任务执行前就已经失效了。

当前采用：

```cpp
using FunctionType = std::decay_t<F>;
using ArgumentsTuple = std::tuple<std::decay_t<Args>...>;
```

其默认行为是：

- 普通左值参数被复制；
- 右值参数被移动；
- 数组和函数类型发生退化；
- 顶层 const 和引用限定被移除。

任务包装器会拥有函数对象和参数的独立副本：

```cpp
[callable = FunctionType(std::forward<F>(function)), arguments = ArgumentsTuple(std::forward<Args>(args)...)]() mutable {
    return std::apply( std::move(callable), std::move(arguments) );
}
```

这种设计可以避免异步任务访问已经失效的局部变量。

## 值传递与引用传递

普通左值默认复制到任务内部：

```cpp
std::string original = "before";

auto future = pool.Submit([](std::string value) {
    value = "inside-task";
    return value;
}, original);
```

任务执行后：

```cpp
future.get() == "inside-task";
original == "before";
```

如果任务需要修改源对象，调用者必须显示使用：`std::ref(object)`。

例如：

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

执行完成后：`value == 42;`。

显式使用 `std::ref` 可以让引用语义更加清晰，同时提醒调用者负责保证被引用对象的生命周期足够长。

## move-only 参数

当前实现支持 `std::unique_ptr` 等不可复制对象：

```cpp
auto future = pool.Submit([](std::unique_ptr<int> value) {
    return *value;
}, std::make_unique<int>(42));
```

右值 `std::unique_ptr` 会被移动到任务保存的参数元组中，任务执行时再移动给用户函数。

这要求任务包装过程中避免不必要的复制。

## packaged_task 和 future

### packaged_task 的职责

**`std::packaged_task` 将一个可调用对象包装成异步任务**，并将执行结果写入共享状态。

```cpp
std::packaged_task<int()> task([] {return 1;});
```

通过：

```cpp
std::future<int> future = task.get_future();
```

可以获得与任务共享状态关联的 `future`，执行 `task()` 后，通过 `future.get()` 可以得到任务返回值。

### 共享状态

`packaged_task` 和 `future` 通过**共享状态**通信：

```
 工作线程
    |
    │ 执行 packaged_task
    ▼
 共享状态
    ▲
    | future.get()
    |
 调用线程
```

共享状态可以保存：

- 任务返回值；
- 任务异常；
- 任务是否已经完成状态；
- broken promise 状态；

### 异常传播

任务抛出异常时：

```cpp
auto future = pool.Submit([]() -> int {
    throw std::runtime_error("task failed");
});
```

异常不会直接从工作线程传播到主线程，也不会导致工作线程退出。

`packaged_task` 会补货异常，并将其保存在共享状态中。

调用 `future.get()` 时，异常会在调用 `get()` 的线程重新抛出：

```cpp
try {
    future.get();
} catch (const std::runtime_error& e) {
    // 处理任务异常
}
```

这样可以将异常处理责任交给任务提交者。

## 为什么 packaged_task 使用 shared_ptr

在 DAY 1 中，任务队列的类型是：

```cpp
using Task = std::function<void()>;
```

在 C++ 17 中，`std::function` 保存的可调用对象**需要可复制**。如果 lambda 直接捕获 **move-only** 的 `packaged_task`，这个 lambda 本身也会成为 move-only 对象，不能直接放入 `std::function`。

因此当前实现使用：

```cpp
auto packaged_task = std::make_shared<std::packaged_task<ReturnType()>>(...);
```

队列中的 lambda 只捕获可复制的 shared_ptr：

```cpp
[packaged_task] {
    (*packaged_task)();
}
```

这样可以继续使用统一的 `std::function<void()>` 任务队列。

## 生命周期状态机

在 DAY 1 中，使用一个布尔变量 `m_stopping` 保存线程池的状态。

在 DAY 2 中改为：

```cpp
enum class RuntimeState {
    Created,
    Running,
    Stopping,
    Stopped
};
```
其中：

- `Created`：表示对象已经开始构造，但工作线程尚未全部准备完成。当前版本中，该状态存在时间非常短，主要用于表达完整生命周期。
- `Running`：线程池正常运行，可以接收新任务。
- `Stopping`：线程池正在关闭，不再接收新任务、工作线程可能仍在执行任务、Shutdown() 可能正在等待工作线程退出。
- `Stopped`：所有工作线程已经被 `join()`，线程池完全停止。

状态转移：

```
Created
   │
   ▼ 
Running
   │
   ▼
Stopping
   │
   ▼
Stopped
```

## Shutdown 语义

`Shutdown()` 表示优雅关闭，具体语义如下：

1. 将状态从 `Running` 修改为 `Stopping`；
2. 停止接收新任务；
3. 唤醒所有等待中的工作线程；
4. 队列中已有任务继续执行；
5. 等待所有工作线程退出；
6. 将状态修改为 `Stopped`。

`Shutdown()` 返回时，可以保证：

- 所有排队任务已经执行完成；
- 所有工作线程已经退出；
- 所有线程都已经被 join()；
- 状态为 Stopped。

## ShutdownNow 语义

`ShutdownNow()` 表示尽快关闭，具体语义如下：

1. 修改状态为 `Stopping`；
2. 停止接收新任务；
3. 清空尚未开始执行的排队任务；
4. 已经开始运行的任务继续执行；
5. 唤醒所有工作线程；
6. 等待正在运行的任务自然结束；
7. 回收工作线程；
8. 将状态修改为 `Stopped`。

必须注意：**`ShutdownNow()` 不等于强制杀死线程**。**C++ 没有安全的通用方式可以从外部强制终止正在运行的线程。**

如果任务正在：

- 持有互斥锁；
- 修改共享数据；
- 分配或释放资源；
- 更新数据结构；
  
强制结束线程可能破坏程序不变量。因此，当前 `ShutdownNow()` 只能丢弃尚未开始执行的任务。

`ShutdownNow()` 清空任务队列后，被丢弃的 `packaged_task` 不会执行。当保存 `packaged_task` 的对象被销毁时，对应共享状态会进入：

```
broken promise
```

此时，调用 `future.get()` 会抛出 `std::future_error`，错误码为 `std::future_errc::broken_promise`

当前版本通过 broken promise 表示排队任务被丢弃，后续实现显式任务取消后，可以定义更清晰的 TaskCancelled 异常。

## WorkerLoop 变化

Day 2 的等待条件：

```cpp
cv_.wait(lock, [this] {
    return
        m_state != RuntimeState::Running ||
        !tasks_.empty();
});
```

工作线程被唤醒的原因有两个：

1. 队列中出现新的任务；
2. 线程池开始关闭；

唤醒后：

```cpp
if (tasks_.empty()) {
    return;
}
```

能执行到这里并且队列为空，表示线程池不再处于 `Running` 状态，因此工作线程可以退出。

如果状态为 Stopping，但队列仍有任务：

```cpp
task = std::move(tasks_.front());
tasks_.pop();
```

工作线程会继续执行任务，从而实现 `Shutdown()` 的排空语义。

## 今日测试覆盖

### 泛型任务测试

- 返回整数；
- 返回字符串；
- 返回 void；
- 普通参数传递；
- 普通左值复制；
- 使用 std::ref 传递引用；
- move-only 参数；
- 用户任务异常传播；
- 一个任务失败后继续执行后续任务；
- 停止后拒绝提交。

### 生命周期测试

- 零工作线程构造失败；
- 构造完成后状态为 Running；
- Shutdown() 执行完全部排队任务；
- 析构函数执行优雅关闭；
- 重复调用 Shutdown()；
- 多线程并发调用 Shutdown()；
- ShutdownNow() 丢弃排队任务；
- ShutdownNow() 不强制终止正在运行的任务；
- 关闭完成后状态为 Stopped。

## 关键 C++ 知识点

### 模板与泛型

- 函数模板；
- 可变参数模板；
- 模板参数推导；
- std::invoke_result_t；
- std::decay_t；
- std::tuple；
- std::apply。

### 移动语义

- 转发引用；
- std::forward；
- std::move；
- move-only 类型；
- 参数所有权转移。

### 异步机制

- std::packaged_task；
- std::future；
- 共享状态；
- 异常跨线程传播；
- broken promise。

### 并发控制

- 状态机；
- 关闭与提交竞争；
- 多线程并发关闭；
- 锁职责划分；
- 条件变量；
- join() 的并发安全。

## 当前版本限制

DAY 2 完成后，MiniRuntime 仍有以下限制：

- 任务队列容量无限；
- 没有背压；
- 没有拒绝策略；
- 没有任务优先级；
- 没有显示任务句柄；
- 没有协作式取消；
- 没有超时取消；
- `ShutdownNow()` 使用 broken promise 表示任务被丢弃；
- 所有工作线程共享一个全局任务队列；
- 没有工作窃取；
- 不支持定时任务；
- 不支持协程；
- 不支持从工作线程内部销毁线程池；
- 尚未通过 ThreadSanitizer 检查。

后续将逐步增加：

- 有界队列
- 拒绝策略
- 协作式取消
- 运行指标
- 每线程本地队列
- 工作窃取

## 验收清单

完成 Day 2 后，应能够独立解释：

- [x] 为什么 Submit() 必须是函数模板；
- [x] 为什么模板实现通常放在头文件；
- [x] std::future 和 std::packaged_task 如何通信；
- [x] 用户任务异常如何传递到调用线程；
- [x] 为什么普通左值默认复制到异步任务；
- [x] 如何显式传递引用参数；
- [x] 如何支持 std::unique_ptr 等 move-only 参数；
- [x] 为什么 packaged_task 使用 shared_ptr 包装；
- [x] 类型擦除发生在什么位置；
- [x] Shutdown() 与 ShutdownNow() 的区别；
- [x] 为什么不能强制杀死正在运行的线程；
- [x] 为什么状态检查和任务入队必须在同一个锁内；
- [x] 为什么不能持有任务队列锁调用 join()；
- [x] m_shutdownMutex 的作用是什么；
- [x] broken promise 是如何产生的。