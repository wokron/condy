/**
 * @file link-cp.cpp
 * @brief Example of file copy using linked read and write
 */

#include "condy/async_operations.hpp"
#include "condy/runtime.hpp"
#include <condy.hpp>
#include <cstdio>
#include <cstring>
#include <sys/types.h>
#include <unistd.h>

static size_t task_num = 64;
static size_t chunk_size = 1024 * 1024l; // 1 MB
static bool use_direct = false;

condy::Coro<void> copy_file_task(size_t task_id, loff_t &offset,
                                 loff_t file_size, void *buffer) {
    using condy::operators::operator>>;

    int buffer_index = static_cast<int>(task_id);

    while (offset < file_size) {
        loff_t current_offset = offset;
        offset += static_cast<loff_t>(chunk_size);

        auto to_copy = std::min(static_cast<loff_t>(chunk_size),
                                file_size - current_offset);
        auto buf = condy::buffer(buffer, to_copy);

        auto aw1 = condy::async_read(
            condy::fixed(0), condy::fixed(buffer_index, buf), current_offset);
        auto aw2 = condy::async_write(
            condy::fixed(1), condy::fixed(buffer_index, buf), current_offset);
        auto [r1, r2] = co_await (std::move(aw1) >> std::move(aw2));

        if (r1 < 0 || r2 < 0) {
            std::fprintf(stderr, "Failed to copy at offset %lld: %d %d\n",
                         (long long)current_offset, r1, r2);
            exit(1);
        }
    }
}

condy::Coro<void> do_file_copy(int infd, int outfd, loff_t size) {
    size_t buffers_size = task_num * chunk_size;
    void *raw_buffer;
    if (posix_memalign(&raw_buffer, 4096, buffers_size) != 0) {
        std::fprintf(stderr, "Failed to allocate aligned buffers: %d\n", errno);
        exit(1);
    }

    auto &fd_table = condy::current_runtime().fd_table();
    fd_table.init(2);
    int fds[2] = {infd, outfd};
    fd_table.update(0, fds, 2);

    auto &buffer_table = condy::current_runtime().buffer_table();
    buffer_table.init(task_num);
    std::vector<iovec> iovs(task_num);
    for (size_t i = 0; i < task_num; i++) {
        iovs[i] = {
            .iov_base = static_cast<char *>(raw_buffer) + i * chunk_size,
            .iov_len = chunk_size,
        };
    }
    buffer_table.update(0, iovs.data(), task_num);

    int r_faddvise = co_await condy::async_fadvise(
        infd, 0, static_cast<off_t>(size), POSIX_FADV_SEQUENTIAL);
    if (r_faddvise < 0) {
        std::fprintf(stderr, "Failed to fadvise input file: %d\n", r_faddvise);
        exit(1);
    }

    std::vector<condy::Task<void>> tasks;
    tasks.reserve(task_num);
    loff_t offset = 0;
    for (size_t i = 0; i < task_num; i++) {
        tasks.emplace_back(condy::co_spawn(
            copy_file_task(i, offset, size,
                           static_cast<char *>(raw_buffer) + i * chunk_size)));
    }
    for (auto &task : tasks) {
        co_await std::move(task);
    }
    free(raw_buffer);
}

condy::Coro<void> co_main(const char *infile, const char *outfile) {
    using condy::operators::operator&&;

    int flags = 0;
    if (use_direct) {
        flags |= O_DIRECT;
    }

    auto [infd, outfd] =
        co_await (condy::async_open(infile, O_RDONLY | flags, 0) &&
                  condy::async_open(outfile, O_WRONLY | O_CREAT | flags, 0644));
    if (infd < 0 || outfd < 0) {
        std::fprintf(stderr, "Failed to open file: %d %d\n", infd, outfd);
        exit(1);
    }

    struct statx statx_buf;
    int r_stat = co_await condy::async_statx(
        AT_FDCWD, infile, AT_STATX_SYNC_AS_STAT, STATX_SIZE, &statx_buf);
    if (r_stat < 0) {
        std::fprintf(stderr, "Failed to statx file: %d\n", r_stat);
        exit(1);
    }

    if (use_direct && (statx_buf.stx_size % 4096 != 0)) {
        std::fprintf(
            stderr,
            "File size %lld is not multiple of 4096 bytes for O_DIRECT\n",
            (long long)statx_buf.stx_size);
        exit(1);
    }

    std::printf("Copy %lld bytes from %s to %s\n",
                (long long)statx_buf.stx_size, infile, outfile);

    auto start = std::chrono::high_resolution_clock::now();

    co_await do_file_copy(infd, outfd, static_cast<loff_t>(statx_buf.stx_size));

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end - start;
    double mbps =
        (static_cast<double>(statx_buf.stx_size) / (1024.0 * 1024.0)) /
        elapsed.count();
    std::printf("Copied %lld bytes in %.2f seconds (%.2f MB/s)\n",
                (long long)statx_buf.stx_size, elapsed.count(), mbps);

    co_await (condy::async_close(infd) && condy::async_close(outfd));
}

void usage(const char *progname) {
    std::fprintf(
        stderr,
        "Usage: %s [-hd] [-t <task_num>] [-c <chunk_size>] <infile> <outfile>\n"
        "  -h               Show this help message\n"
        "  -d               Use O_DIRECT for file operations\n"
        "  -t <task_num>    Number of concurrent copy tasks\n"
        "  -c <chunk_size>  Size of each copy chunk\n",
        progname);
}

size_t get_chunk_size(const char *arg) {
    size_t len = std::strlen(arg);
    int suffix = std::tolower(arg[len - 1]);
    size_t multiplier = 1;
    if (suffix == 'k') {
        multiplier = 1024;
        len -= 1;
    } else if (suffix == 'm') {
        multiplier = 1024l * 1024;
        len -= 1;
    } else if (suffix == 'g') {
        multiplier = 1024l * 1024 * 1024;
        len -= 1;
    }
    return std::stoul(std::string(arg, len)) * multiplier;
}

int main(int argc, char **argv) noexcept(false) {
    int opt;
    while ((opt = getopt(argc, argv, "ht:c:d")) != -1) {
        switch (opt) {
        case 't':
            task_num = std::stoul(optarg);
            break;
        case 'c':
            chunk_size = get_chunk_size(optarg);
            break;
        case 'd':
            use_direct = true;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (argc - optind < 2) {
        usage(argv[0]);
        return 1;
    }

    auto options = condy::RuntimeOptions().sq_size(task_num * 2);
    condy::Runtime runtime(options);
    condy::sync_wait(runtime, co_main(argv[optind], argv[optind + 1]));
    return 0;
}