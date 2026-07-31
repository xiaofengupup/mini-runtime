#include "minirt/work_stealing_queue.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

TEST(WorkStealingQueueTest, OwnerPopsFromBack)
{
    minirt::WorkStealingQueue<int> queue;

    queue.Push(1);
    queue.Push(2);
    queue.Push(3);

    int value = 0;

    ASSERT_TRUE(queue.TryPop(value));
    EXPECT_EQ(value, 3);

    ASSERT_TRUE(queue.TryPop(value));
    EXPECT_EQ(value, 2);

    ASSERT_TRUE(queue.TryPop(value));
    EXPECT_EQ(value, 1);

    EXPECT_FALSE(queue.TryPop(value));
}

TEST(WorkStealingQueueTest, ThiefStealsFromFront)
{
    minirt::WorkStealingQueue<int> queue;

    queue.Push(1);
    queue.Push(2);
    queue.Push(3);

    int value = 0;

    ASSERT_TRUE(queue.TrySteal(value));
    EXPECT_EQ(value, 1);

    ASSERT_TRUE(queue.TrySteal(value));
    EXPECT_EQ(value, 2);

    ASSERT_TRUE(queue.TrySteal(value));
    EXPECT_EQ(value, 3);

    EXPECT_FALSE(queue.TrySteal(value));
}

TEST(WorkStealingQueueTest, OwnerAndThiefUseOppositeEnds)
{
    minirt::WorkStealingQueue<int> queue;

    queue.Push(1);
    queue.Push(2);
    queue.Push(3);
    queue.Push(4);

    int ownerValue = 0;
    int stolenValue = 0;

    ASSERT_TRUE(queue.TryPop(ownerValue));

    ASSERT_TRUE(queue.TrySteal(stolenValue));

    EXPECT_EQ(ownerValue, 4);
    EXPECT_EQ(stolenValue, 1);
    EXPECT_EQ(queue.Size(), 2U);
}

TEST(WorkStealingQueueTest, DrainMovesAllPendingItems)
{
    minirt::WorkStealingQueue<int> queue;

    queue.Push(1);
    queue.Push(2);
    queue.Push(3);

    std::vector<int> drained;

    const std::size_t count = queue.DrainTo(drained);

    EXPECT_EQ(count, 3U);
    EXPECT_EQ(drained.size(), 3U);
    EXPECT_TRUE(queue.Empty());

    int value = 0;
    EXPECT_FALSE(queue.TryPop(value));
}

}  // namespace