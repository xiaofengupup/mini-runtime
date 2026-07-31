/**
 * 任务拒绝策略
 */
#pragma once

#include <stdexcept>

namespace minirt {

/**
 * 当任务队列已满时采用的处理策略
 */
enum class RejectionPolicy {
    Block,      // 阻塞提交线程，直到任务队列出现空间
    Reject,     // 立即拒绝任务，并抛出 TaskRejected
    CallerRuns,  // 由提交任务的线程直接执行任务
};

/**
 * 任务无法被线程池接受时抛出的异常
 * 
 * TaskRejected 继承 std::runtime_error，因此 Day 2 中
 * EXPECT_THROW(..., std::runtime_error) 的测试仍然可以通过。
 */
class TaskRejected : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

} // namespace minirt
