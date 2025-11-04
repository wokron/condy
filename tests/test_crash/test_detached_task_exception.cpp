#define CRASH_TEST

#include "condy/async_operations.hpp"
#include "condy/sync_wait.hpp"
#include "condy/task.hpp"

condy::Coro<int> simple_task() {
    throw std::runtime_error("Intentional exception");
    co_return 42;
}

condy::Coro<int> co_main() {
    condy::co_spawn(simple_task()).detach();
    __kernel_timespec ts{
        .tv_sec = 0,
        .tv_nsec = 10 * 1000 * 1000 // 10ms
    };
    co_await condy::async_timeout(&ts, 0, 0);
    co_return 0;
}

int main() {
    condy::sync_wait(co_main());
    return 0;
}