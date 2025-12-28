#include "condy/async_operations.hpp"
#include "condy/coro.hpp"
#include "condy/runtime.hpp"
#include "condy/sync_wait.hpp"
#include "condy/task.hpp"
#include <doctest/doctest.h>

TEST_CASE("test runtime_options - event_interval") {
    condy::RuntimeOptions options;
    options.event_interval(10);
    condy::Runtime runtime(options);

    auto task_func = [&]() -> condy::Coro<void> {
        for (int i = 0; i < 5; i++) {
            co_await condy::async_nop();
        }
    };

    auto func = [&]() -> condy::Coro<void> {
        std::vector<condy::Task<void>> tasks;
        tasks.reserve(20);
        for (int i = 0; i < 20; i++) {
            tasks.push_back(condy::co_spawn(task_func()));
        }

        for (auto &t : tasks) {
            co_await std::move(t);
        }
    };

    condy::sync_wait(func());
}

TEST_CASE("test runtime_options - enable_iopoll") {
    char name[32] = "XXXXXX";
    int fd = mkstemp(name);
    REQUIRE(fd >= 0);

    std::string msg = "Hello, world!";
    ssize_t written = write(fd, msg.data(), msg.size());
    REQUIRE(written == (ssize_t)msg.size());
    fsync(fd);
    close(fd);

    fd = open(name, O_RDWR | O_DIRECT);
    REQUIRE(fd >= 0);

    auto d = condy::defer([&]() {
        close(fd);
        unlink(name);
    });

    condy::RuntimeOptions options;
#if !IO_URING_CHECK_VERSION(2, 9) // >= 2.9
    options.enable_iopoll(/*hybrid=*/true);
#else
    options.enable_iopoll(/*hybrid=*/false);
#endif
    condy::Runtime runtime(options);

    alignas(4096) char buffer[4096];

    auto func = [&]() -> condy::Coro<void> {
        int n = co_await condy::async_read(fd, condy::buffer(buffer), 0);
        REQUIRE(n == static_cast<int>(msg.size()));
        REQUIRE(std::string_view(buffer, msg.size()) == msg);
    };

    condy::sync_wait(func());
}

TEST_CASE("test runtime_options - enable_sqpoll") {
    condy::RuntimeOptions options;
    options.enable_sqpoll(2000, 0).sq_size(8).cq_size(16);
    condy::Runtime runtime(options);

    auto task_func = [&]() -> condy::Coro<void> {
        for (int i = 0; i < 5; i++) {
            co_await condy::async_nop();
        }
    };

    auto func = [&]() -> condy::Coro<void> {
        std::vector<condy::Task<void>> tasks;
        tasks.reserve(1000);
        for (int i = 0; i < 1000; i++) {
            tasks.push_back(condy::co_spawn(task_func()));
        }

        for (auto &t : tasks) {
            co_await std::move(t);
        }
    };

    condy::sync_wait(func());
}

TEST_CASE("test runtime_options - enable_defer_taskrun") {
    condy::RuntimeOptions options;
    options.enable_defer_taskrun().sq_size(8).cq_size(16);
    condy::Runtime runtime(options);

    auto task_func = [&]() -> condy::Coro<void> {
        for (int i = 0; i < 5; i++) {
            co_await condy::async_nop();
        }
    };

    auto func = [&]() -> condy::Coro<void> {
        std::vector<condy::Task<void>> tasks;
        tasks.reserve(1000);
        for (int i = 0; i < 1000; i++) {
            tasks.push_back(condy::co_spawn(task_func()));
        }

        for (auto &t : tasks) {
            co_await std::move(t);
        }
    };

    condy::sync_wait(func());
}

TEST_CASE("test runtime_options - enable_attach_wq") {
    condy::RuntimeOptions options1;
    options1.enable_sqpoll(2000).sq_size(8).cq_size(16);
    condy::Runtime runtime1(options1);

    condy::RuntimeOptions options2;
    options2.enable_sqpoll().enable_attach_wq(runtime1).sq_size(8).cq_size(16);
    condy::Runtime runtime2(options2);

    auto task_func = [&]() -> condy::Coro<void> {
        for (int i = 0; i < 5; i++) {
            co_await condy::async_nop();
        }
    };

    auto func = [&]() -> condy::Coro<void> {
        std::vector<condy::Task<void>> tasks;
        tasks.reserve(1000);
        for (int i = 0; i < 1000; i++) {
            tasks.push_back(condy::co_spawn(task_func()));
        }

        for (auto &t : tasks) {
            co_await std::move(t);
        }
    };

    std::thread rt2([&] { condy::sync_wait(runtime2, func()); });

    condy::sync_wait(runtime1, func());

    rt2.join();
}

TEST_CASE("test runtime_options - enable_coop_taskrun") {
    condy::RuntimeOptions options;
    options.enable_coop_taskrun(true).sq_size(8).cq_size(16);
    condy::Runtime runtime(options);

    auto task_func = [&]() -> condy::Coro<void> {
        for (int i = 0; i < 5; i++) {
            co_await condy::async_nop();
        }
    };

    auto func = [&]() -> condy::Coro<void> {
        std::vector<condy::Task<void>> tasks;
        tasks.reserve(1000);
        for (int i = 0; i < 1000; i++) {
            tasks.push_back(condy::co_spawn(task_func()));
        }

        for (auto &t : tasks) {
            co_await std::move(t);
        }
    };

    condy::sync_wait(func());
}

TEST_CASE("test runtime_options - enable_sqe128 & enable_cqe32") {
    condy::RuntimeOptions options;
    options.enable_sqe128().enable_cqe32();
    condy::Runtime runtime(options);

    auto task_func = [&]() -> condy::Coro<void> {
        for (int i = 0; i < 5; i++) {
            co_await condy::async_nop();
        }
    };

    auto func = [&]() -> condy::Coro<void> {
        std::vector<condy::Task<void>> tasks;
        tasks.reserve(1000);
        for (int i = 0; i < 1000; i++) {
            tasks.push_back(condy::co_spawn(task_func()));
        }

        for (auto &t : tasks) {
            co_await std::move(t);
        }
    };

    condy::sync_wait(func());
}

TEST_CASE("test runtime_options - enable_no_mmap") {
    void *data = mmap(nullptr, 4096l * 2, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    REQUIRE(data != MAP_FAILED);
    auto unmap = condy::defer([data]() { munmap(data, 4096l * 2); });

    condy::RuntimeOptions options;
    options.enable_no_mmap(data, 4096l * 2).sq_size(8).cq_size(16);
    condy::Runtime runtime(options);

    auto task_func = [&]() -> condy::Coro<void> {
        for (int i = 0; i < 5; i++) {
            co_await condy::async_nop();
        }
    };

    auto func = [&]() -> condy::Coro<void> {
        std::vector<condy::Task<void>> tasks;
        tasks.reserve(1000);
        for (int i = 0; i < 1000; i++) {
            tasks.push_back(condy::co_spawn(task_func()));
        }

        for (auto &t : tasks) {
            co_await std::move(t);
        }
    };

    condy::sync_wait(func());
}