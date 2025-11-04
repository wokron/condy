#define CRASH_TEST

#include "condy/async_operations.hpp"
#include "condy/channel.hpp"
#include "condy/sync_wait.hpp"
#include "condy/task.hpp"

condy::Coro<int> simple_task(condy::Channel<int> &ch) {
    co_return co_await ch.pop();
}

condy::Coro<void> early_exit_task() {
    condy::Channel<int> ch(1);
    condy::co_spawn(simple_task(ch)).detach();
    __kernel_timespec ts{
        .tv_sec = 0,
        .tv_nsec = 10 * 1000 * 1000 // 10ms
    };
    co_await condy::async_timeout(&ts, 0, 0);
    co_return;
}

condy::Coro<int> co_main() {
    condy::co_spawn(early_exit_task()).detach();
    co_return 0;
}

int main() {
    condy::sync_wait(co_main());
    return 0;
}