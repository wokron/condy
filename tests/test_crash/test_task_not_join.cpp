#define CRASH_TEST

#include "condy/sync_wait.hpp"
#include "condy/task.hpp"

condy::Coro<int> simple_task() { co_return 42; }

condy::Coro<int> co_main() {
    auto t = condy::co_spawn(simple_task());
    // co_await std::move(t); // Missing co_await should cause a crash
    co_return 0;
}

int main() {
    condy::sync_wait(co_main());
    return 0;
}