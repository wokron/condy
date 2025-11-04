#include "condy/async_operations.hpp"
#include "condy/runtime.hpp"
#include "condy/runtime_options.hpp"
#include <condy.hpp>
#include <cstddef>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

int get_file_size(int fd, off_t *size) {
    struct stat st;

    if (fstat(fd, &st) < 0)
        return -1;
    if (S_ISREG(st.st_mode)) {
        *size = st.st_size;
        return 0;
    } else if (S_ISBLK(st.st_mode)) {
        unsigned long long bytes;

        if (ioctl(fd, BLKGETSIZE64, &bytes) != 0)
            return -1;

        *size = bytes;
        return 0;
    }

    return -1;
}

condy::Coro<> copy_file_task(int infd, int outfd, off_t &offset,
                             off_t file_size, size_t chunk_size) {
    using condy::operators::operator>>;
    std::unique_ptr<char[]> buffer = std::make_unique<char[]>(chunk_size);

    while (offset < file_size) {
        off_t current_offset = offset;
        size_t to_copy =
            std::min(chunk_size, static_cast<size_t>(file_size - offset));
        offset += to_copy;

        auto [r1, r2] = co_await (
            condy::async_read(infd, buffer.get(), to_copy, current_offset) >>
            condy::async_write(outfd, buffer.get(), to_copy, current_offset));
        if (r1 < 0 || r2 < 0) {
            std::fprintf(stderr,
                         "Error during file copy at offset %ld: "
                         "read %s, write %s\n",
                         current_offset, r1 < 0 ? std::strerror(-r1) : "ok",
                         r2 < 0 ? std::strerror(-r2) : "ok");
            exit(1);
        }
    }
}

condy::Coro<> co_main(const char *infile, const char *outfile) {
    using condy::operators::operator&&;

    struct statx statx_buf;
    auto [infd, outfd, r] =
        co_await (condy::async_openat(AT_FDCWD, infile, O_RDONLY, 0) &&
                  condy::async_openat(AT_FDCWD, outfile,
                                      O_WRONLY | O_CREAT | O_TRUNC, 0644) &&
                  condy::async_statx(AT_FDCWD, infile, AT_STATX_SYNC_AS_STAT,
                                     STATX_SIZE, &statx_buf));
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
    constexpr size_t CHUNK_SIZE = 32 * 1024; // 32 KB

    std::vector<condy::Task<>> tasks;
    for (int i = 0; i < 32; i++) {
        tasks.emplace_back(condy::co_spawn(
            copy_file_task(infd, outfd, offset, file_size, CHUNK_SIZE)));
    }

    for (auto &task : tasks) {
        co_await std::move(task);
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <infile> <outfile>\n", argv[0]);
        return 1;
    }

    auto options =
        condy::SingleThreadOptions().sq_size(64).cq_size(128).submit_batch_size(
            64);
    condy::SingleThreadRuntime runtime(options);
    condy::sync_wait(runtime, co_main(argv[1], argv[2]));
    return 0;
}