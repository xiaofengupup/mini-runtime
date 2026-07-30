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

