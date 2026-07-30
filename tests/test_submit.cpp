#include "minirt/thread_pool.h"

#include <gtest/gtest.h>
#include <atomic>
#include <string>

namespace {

TEST(SubmitTest, ReturnIntegerValue)
{
    minirt::ThreadPool pool(2);

    auto future = pool.Submit([](int lhs, int rhs) {
        return lhs + rhs;
    }, 10, 20);

    EXPECT_EQ(future.get(), 30);

    pool.Shutdown();
}

TEST(SubmitTest, SupportsVoidReturnType)
{
    minirt::ThreadPool pool(2);

    std::atomic<bool> executed {false};
    std::future<void> future = pool.Submit([&executed] {
        executed.store(true, std::memory_order_relaxed);
    });

    future.get();

    EXPECT_TRUE(executed.load(std::memory_order_relaxed));

    pool.Shutdown();
}

TEST(SubmitTest, SupportsStringArgumentsAndReturnValue)
{
    minirt::ThreadPool pool(2);

    auto future = pool.Submit([](std::string prefix, int value) {
        return prefix + std::to_string(value);
    }, std::string("task-"), 1);

    EXPECT_EQ(future.get(), "task-1");

    pool.Shutdown();
}

TEST(SubmitTest, PropagatesTaskExceptionThroughFuture)
{
    minirt::ThreadPool pool(2);

    auto future = pool.Submit([]() -> int {
        throw std::runtime_error("expected failure");
    });

    EXPECT_THROW(future.get(), std::runtime_error);

    // 某个任务抛出异常后，线程池仍然可以继续执行
    auto nextFuture = pool.Submit([]{
        return 100;
    });
    EXPECT_EQ(nextFuture.get(), 100);

    pool.Shutdown();
}

TEST(SubmitTest, SupportsMoveOnlyArgument)
{
    minirt::ThreadPool pool(2);

    auto future = pool.Submit([](std::unique_ptr<int> value) {
        return *value;
    }, std::make_unique<int>(1));

    EXPECT_EQ(future.get(), 1);

    pool.Shutdown();
}

TEST(SubmitTest, PassesLvalueByReferenceUsingStdRef)
{
    minirt::ThreadPool pool(2);

    int value = 0;
    auto future = pool.Submit([](int& target) {
        target = 42;
    }, std::ref(value));

    future.get();

    EXPECT_EQ(value, 42);

    pool.Shutdown();
}

TEST(SubmitTest, CopiesNormalLvalueArguments)
{
    minirt::ThreadPool pool(2);

    std::string original = "before";
    auto future = pool.Submit([](std::string value) {
        value = "inside-task";
        return value;
    }, original);

    EXPECT_EQ(future.get(), "inside-task");
    EXPECT_EQ(original, "before");  // 普通左值会复制到异步任务中，不会修改原变量

    pool.Shutdown();
}

TEST(SubmitTest, PropagatesEmptyFunctionException)
{
    minirt::ThreadPool pool(2);
    
    std::function<void()> emptyFunction;
    auto future = pool.Submit(emptyFunction);

    /* 空 std::function 在执行时抛出 bad_function_call，packaged_task 会将它保存到 future*/
    EXPECT_THROW(future.get(), std::bad_function_call);
}

TEST(SubmitTest, RejectsSubmissionAfterShutdown)
{
    minirt::ThreadPool pool(2);

    pool.Shutdown();

    EXPECT_THROW(pool.Submit([] {return 0; }), std::runtime_error);
}

}
