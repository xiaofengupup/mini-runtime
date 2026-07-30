/**
 * 线程池配置
 */
#pragma once

#include "minirt/rejection_policy.h"
#include <cstddef>

namespace minirt {

struct ThreadPoolOptions {
    // 工作线程数量
    std::size_t threadCount {1};

    // 全局等待队列的最大容量，不包括已经被工作线程取出、正在运行的任务
    std::size_t queueCapacity {1024};

    // 队列已满时的处理策略
    RejectionPolicy rejectPolicy {RejectionPolicy::Block};
};

} // namespace minirt
