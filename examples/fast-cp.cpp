#include <condy.hpp>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

constexpr size_t TASK_NUM = 64;
constexpr size_t BLOCK_ALIGN = 512;
constexpr size_t CHUNK_SIZE = 256 * 1024 * 1024; // 256 MiB

struct raw_deleter {
    void operator()(void *ptr) const { std::free(ptr); }
};

condy::Coro<> copy_file_task(int infd, int outfd, off_t &offset,
                             off_t file_size) {
    using condy::operators::operator>>;

    void *raw_ptr;
    if (posix_memalign(&raw_ptr, BLOCK_ALIGN, CHUNK_SIZE) != 0) {
        std::fprintf(stderr, "posix_memalign failed\n");
        exit(1);
    }
    std::unique_ptr<char[], raw_deleter> buffer(static_cast<char *>(raw_ptr));

    while (offset < file_size) {
        off_t current_offset = offset;
        bool partial_copy = current_offset + CHUNK_SIZE > file_size;
        offset += CHUNK_SIZE;

        int r1, r2;
        if (!partial_copy) {
            std::tie(r1, r2) = co_await (
                condy::async_read(infd, condy::buffer(buffer.get(), CHUNK_SIZE),
                                  current_offset) >>
                condy::async_write(outfd,
                                   condy::buffer(buffer.get(), CHUNK_SIZE),
                                   current_offset));

        } else {
            r1 = co_await condy::async_read(
                infd, condy::buffer(buffer.get(), CHUNK_SIZE), current_offset);
            r2 = co_await condy::async_write(
                outfd, condy::buffer(buffer.get(), CHUNK_SIZE), current_offset);
        }
        if (r1 < 0) {
            std::fprintf(stderr, "Read error at offset %lld: %s\n",
                         (long long)current_offset, std::strerror(-r1));
            exit(1);
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

    std::vector<condy::Task<>> tasks;
    for (int i = 0; i < TASK_NUM; i++) {
        tasks.emplace_back(
            condy::co_spawn(copy_file_task(infd, outfd, offset, file_size)));
    }

    for (auto &task : tasks) {
        co_await std::move(task);
    }

    ftruncate(outfd, file_size);
    co_await (condy::async_close(infd) && condy::async_close(outfd));
}

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <infile> <outfile>\n", argv[0]);
        return 1;
    }

    auto options = condy::RuntimeOptions()
                       .sq_size(2 * TASK_NUM)
                       .cq_size(4 * TASK_NUM)
                       .submit_batch_size(2 * TASK_NUM);
    condy::Runtime runtime(options);
    condy::sync_wait(runtime, co_main(argv[1], argv[2]));
    return 0;
}