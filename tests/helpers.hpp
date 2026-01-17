#pragma once

#include <cerrno>
#include <condy/async_operations.hpp>
#include <cstddef>
#include <cstring>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace {

inline int create_accept_socket() {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(sockfd >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // Let the OS choose the port

    int r = bind(sockfd, (sockaddr *)&addr, sizeof(addr));
    REQUIRE(r == 0);

    r = listen(sockfd, 1);
    REQUIRE(r == 0);

    return sockfd;
}

inline void create_tcp_socketpair(int sv[2]) {
    int r;
    int listener = create_accept_socket();

    sockaddr_in addr{};
    socklen_t addrlen = sizeof(addr);
    r = getsockname(listener, (sockaddr *)&addr, &addrlen);
    REQUIRE(r == 0);

    sv[0] = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(sv[0] >= 0);

    r = connect(sv[0], (sockaddr *)&addr, sizeof(addr));
    REQUIRE(r == 0);

    sv[1] = accept(listener, nullptr, nullptr);
    REQUIRE(sv[1] >= 0);

    close(listener);
}

inline std::string generate_data(size_t size) {
    std::string data;
    data.resize(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<char>('A' + (i % 26));
    }
    return data;
}

class BlkDevice {
public:
    BlkDevice() {
        char buf[] = "blkdevXXXXXX";
        int fd = mkstemp(buf);
        if (fd < 0) {
            return;
        }

        file_path_ = buf;

        // truncate to 1MB
        if (ftruncate(fd, 1024l * 1024l) != 0) {
            close(fd);
            file_path_.clear();
            return;
        }

        close(fd);

        FILE *f = popen("losetup -f", "r");
        if (!f) {
            return;
        }

        char dev_path[256] = {0};
        if (fgets(dev_path, sizeof(dev_path), f) == nullptr) {
            pclose(f);
            return;
        }
        pclose(f);

        path_ = dev_path;
        // remove trailing newline
        if (!path_.empty() && path_.back() == '\n') {
            path_.pop_back();
        }

        int r = system(("losetup " + path_ + " " + file_path_).c_str());
        if (r != 0) {
            path_.clear();
            return;
        }
    }

    ~BlkDevice() {
        if (!path_.empty()) {
            // detach loop device
            int r = system(("losetup -d " + path_).c_str());
            if (r != 0) {
                MESSAGE("Warning: failed to detach loop device " << path_);
            }
        }
        if (!file_path_.empty()) {
            int r = unlink(file_path_.c_str());
            if (r != 0) {
                MESSAGE("Warning: failed to unlink file " << file_path_);
            }
        }
    }

    BlkDevice(const BlkDevice &) = delete;
    BlkDevice &operator=(const BlkDevice &) = delete;
    BlkDevice(BlkDevice &&) = delete;
    BlkDevice &operator=(BlkDevice &&) = delete;

    const std::string &path() const { return path_; }

private:
    std::string file_path_;
    std::string path_;
};

} // namespace
