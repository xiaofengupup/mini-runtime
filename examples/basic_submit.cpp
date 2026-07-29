#include "minirt/thread_pool.h"
#include <iostream>
#include <atomic>

int main()
{
    minirt::ThreadPool pool(4);
    std::atomic<int> counter {0};

    for (int i = 0; i < 1000; ++i) {
        pool.Submit([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }

    pool.Stop(); // Stop() 会等待已经提交的任务执行完成。

    std::cout << "counter = " << counter.load() << std::endl;
    if (counter.load() != 1000) {
        std::cerr << "Unexpected counter value\n";
        return 1;
    }

    std::cout << "Basic ThreadPool example passed\n";
    return 0;
}