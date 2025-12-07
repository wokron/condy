#include "condy/runtime.hpp"
#include <condy.hpp>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

constexpr size_t TASK_NUM = 64;
constexpr size_t CHUNK_SIZE = 128 * 1024;

condy::Coro<> copy_file_task(off_t &offset, off_t file_size, void *ptr) {
    using condy::operators::operator>>;

    auto buffer = condy::buffer(ptr, CHUNK_SIZE);

    auto fixed_buffer = condy::fixed(0, buffer);
    auto infd = condy::fixed(0);
    auto outfd = condy::fixed(1);

    while (offset < file_size) {
        off_t current_offset = offset;
        offset += CHUNK_SIZE;

        auto [r1, r2] =
            co_await (condy::async_read(infd, fixed_buffer, current_offset) >>
                      condy::async_write(outfd, fixed_buffer, current_offset));
        if (r1 < 0) {
            std::fprintf(stderr, "Read error at offset %lld: %s\n",
                         (long long)current_offset, std::strerror(-r1));
            exit(1);
        }
        if (static_cast<size_t>(r1) < CHUNK_SIZE) {
            r2 = co_await condy::async_write(
                outfd, condy::fixed(0, condy::buffer(ptr, r1)), current_offset);
        }
        if (r2 < 0) {
            std::fprintf(stderr, "Write error at offset %lld: %s\n",
                         (long long)current_offset, std::strerror(-r2));
            exit(1);
        }
    }
}

condy::Coro<> co_main(const char *infile, const char *outfile) {
    using condy::operators::operator&&;

    auto start = std::chrono::high_resolution_clock::now();

    struct statx statx_buf;
    auto [infd, outfd, r] = co_await (
        condy::async_openat(AT_FDCWD, infile, O_RDONLY | O_DIRECT, 0) &&
        condy::async_openat(AT_FDCWD, outfile,
                            O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0644) &&
        condy::async_statx(AT_FDCWD, infile, AT_STATX_SYNC_AS_STAT, STATX_SIZE,
                           &statx_buf));
    if (infd < 0) {
        std::fprintf(stderr, "Failed to open input file '%s': %s\n", infile,
                     std::strerror(-infd));
        exit(1);
    }
    if (outfd < 0) {
        std::fprintf(stderr, "Failed to open output file '%s': %s\n", outfile,
                     std::strerror(-outfd));
        exit(1);
    }
    if (r < 0) {
        std::fprintf(stderr, "Failed to stat input file '%s': %s\n", infile,
                     std::strerror(-r));
        exit(1);
    }

    off_t file_size = statx_buf.stx_size;
    off_t offset = 0;

    void *raw_buffer =
        mmap(nullptr, TASK_NUM * CHUNK_SIZE, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (raw_buffer == MAP_FAILED) {
        std::fprintf(stderr, "Failed to allocate aligned buffer: %s\n",
                     std::strerror(errno));
        exit(1);
    }

    auto &fd_table = condy::current_fd_table();
    fd_table.init(2);
    fd_table.register_fd(0, infd);
    fd_table.register_fd(1, outfd);

    auto &buffer_table = condy::current_buffer_table();
    buffer_table.init(1);
    buffer_table.register_buffer(
        0, condy::buffer(raw_buffer, TASK_NUM * CHUNK_SIZE));

    std::vector<condy::Task<>> tasks;
    for (size_t i = 0; i < TASK_NUM; i++) {
        char *buffer = static_cast<char *>(raw_buffer) + i * CHUNK_SIZE;
        tasks.emplace_back(
            condy::co_spawn(copy_file_task(offset, file_size, buffer)));
    }

    for (auto &task : tasks) {
        co_await std::move(task);
    }

    co_await (condy::async_close(condy::fixed(0)) &&
              condy::async_close(condy::fixed(1)));

    munmap(raw_buffer, TASK_NUM * CHUNK_SIZE);

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end - start;
    double mb_copied = static_cast<double>(file_size) / (1024.0 * 1024.0);
    std::printf("Copied %.2f MiB in %.2f seconds (%.2f MiB/s)\n", mb_copied,
                elapsed.count(), mb_copied / elapsed.count());
    co_return;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <infile> <outfile>\n", argv[0]);
        return 1;
    }

    auto options =
        condy::RuntimeOptions().sq_size(2 * TASK_NUM).cq_size(4 * TASK_NUM);
    condy::Runtime runtime(options);
    condy::sync_wait(runtime, co_main(argv[1], argv[2]));

    return 0;
}