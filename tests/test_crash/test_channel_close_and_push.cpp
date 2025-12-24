#define CRASH_TEST

#include "condy/channel.hpp"
#include "condy/sync_wait.hpp"

condy::Coro<void> co_main() {
    condy::Channel<int> ch(1);
    co_await ch.push(42);
    ch.push_close();
    [[maybe_unused]] int r = co_await ch.pop(); // ok
    assert(r == 42);
    r = co_await ch.pop(); // ok
    assert(r == 0);

    co_await ch.push(43); // will crash since channel is closed
}

int main() {
    condy::sync_wait(co_main());
    return 0;
}