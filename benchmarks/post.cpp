#include <condy.hpp>

condy::Coro<void> test_post(size_t times) {
    for (size_t i = 0; i < times; ++i) {
        co_await condy::co_switch(condy::current_runtime());
    }
}

int main() noexcept(false) {
    const size_t times = 100'000'000;

    auto start = std::chrono::high_resolution_clock::now();
    condy::sync_wait(test_post(times));
    auto end = std::chrono::high_resolution_clock::now();

    auto duration =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
            .count();
    printf("Performed %zu context switches in %ld ns\n", times, duration);
    printf("Average time per context switch: %.2f ns\n",
           static_cast<double>(duration) / times);
}