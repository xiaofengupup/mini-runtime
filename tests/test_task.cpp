#include "minirt/task.h"

#include <gtest/gtest.h>
#include <memory>

namespace {

TEST(MoveOnlyTaskTest, EmptyTaskThrowsBadFunctionCall)
{
    minirt::MoveOnlyTask task;

    EXPECT_FALSE(task);
    EXPECT_TRUE(task == nullptr);
    EXPECT_THROW(task(), std::bad_function_call);
}

TEST(MoveOnlyTaskTest, ExecutesMoveOnlyCallable)
{
    auto value = std::make_unique<int>(41);
    int result = 0;

    minirt::MoveOnlyTask task([value = std::move(value), &result] {
        result = *value + 1;
    });

    ASSERT_TRUE(task);

    task();

    EXPECT_EQ(result, 42);
}

TEST(MoveOnlyTaskTest, SupportsMoveConstruction)
{
    int callCount = 0;
    minirt::MoveOnlyTask original([&callCount] {
        ++callCount;
    });

    minirt::MoveOnlyTask moved(std::move(original));

    EXPECT_FALSE(original);
    ASSERT_TRUE(moved);

    moved();

    EXPECT_EQ(callCount, 1);
}

TEST(MoveOnlyTaskTest, SupportsMoveAssignment)
{
    int result = 0;
    minirt::MoveOnlyTask task;

    task = minirt::MoveOnlyTask([&result] {
        result = 7;
    });

    ASSERT_TRUE(task);

    task();

    EXPECT_EQ(result, 7);
}

} // namespace
