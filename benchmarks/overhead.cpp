#include <condy.hpp>

void run_raw_nop(size_t times) {
    io_uring ring;
    io_uring_queue_init(256, &ring,
                        IORING_SETUP_CLAMP | IORING_SETUP_SINGLE_ISSUER);

    for (size_t i = 0; i < times; ++i) {
        io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        io_uring_prep_nop(sqe);
        io_uring_submit_and_wait(&ring, 1);
        io_uring_cqe *cqe;
        io_uring_peek_cqe(&ring, &cqe);
        io_uring_cqe_seen(&ring, cqe);
    }

    io_uring_queue_exit(&ring);
}

condy::Coro<void> run_condy_nop_coro(size_t times) {
    for (size_t i = 0; i < times; ++i) {
        co_await condy::async_nop();
    }
}

void run_condy_nop(size_t times) {
    condy::Runtime runtime(
        condy::RuntimeOptions{}.sq_size(256).cq_size(512).submit_batch_size(
            256));
    condy::sync_wait(runtime, run_condy_nop_coro(times));
}

// This is the worst case, normal workloads could batch these operations
int main() {
    const size_t iterations = 10'000'000;

    long duration_raw, duration_condy;

    {
        auto start = std::chrono::high_resolution_clock::now();
        run_condy_nop(iterations);
        auto end = std::chrono::high_resolution_clock::now();
        duration_condy =
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
                .count();
        std::printf("Condy NOP: %zu iterations took %ld ns (%ld ns per op)\n",
                    iterations, duration_condy, duration_condy / iterations);
    }

    {
        auto start = std::chrono::high_resolution_clock::now();
        run_raw_nop(iterations);
        auto end = std::chrono::high_resolution_clock::now();
        duration_raw =
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
                .count();
        std::printf("Raw NOP: %zu iterations took %ld ns (%ld ns per op)\n",
                    iterations, duration_raw, duration_raw / iterations);
    }

    long overhead = duration_condy - duration_raw;
    long overhead_per_op = overhead / iterations;
    std::printf("Overhead: %ld ns per operation (%.2f%%)\n", overhead_per_op,
                (static_cast<double>(overhead) / duration_raw) * 100.0);

    return 0;
}