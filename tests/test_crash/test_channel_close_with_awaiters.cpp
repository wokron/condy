#define CRASH_TEST

#include "condy/channel.hpp"
#include "condy/runtime.hpp"
#include "condy/sync_wait.hpp"
#include "condy/task.hpp"

condy::Coro<int> simple_task(condy::Channel<int> &ch) {
    for (;;) {
        co_await ch.push(42);
    }
}

condy::Coro<void> co_main() {
    condy::Channel<int> ch(1);
    condy::co_spawn(simple_task(ch)).detach();
    // Let simple_task run and await on push
    co_await condy::co_switch(condy::current_runtime());

    ch.push_close(); // Will crash since simple_task is awaiting on push
    co_return;
}

int main() {
    condy::sync_wait(co_main());
    return 0;
}