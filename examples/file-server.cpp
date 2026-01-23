/**
 * @file file-server.cpp
 * @brief Simple HTTP file server using condy library
 */

#include "condy/buffers.hpp"
#include <arpa/inet.h>
#include <condy.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <linux/stat.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static std::string bind_address = "0.0.0.0";
static std::string serve_directory = ".";
static uint16_t port = 8080;

static const char HTTP_200_TEMPLATE[] = "HTTP/1.1 200 OK\r\n"
                                        "Content-Length: %d\r\n"
                                        "\r\n";

static const char HTTP_400[] = "HTTP/1.1 400 Bad Request\r\n"
                               "Content-Length: 0\r\n"
                               "\r\n";

static const char HTTP_404[] = "HTTP/1.1 404 Not Found\r\n"
                               "Content-Length: 0\r\n"
                               "\r\n";

static const char HTTP_500[] = "HTTP/1.1 500 Internal Server Error\r\n"
                               "Content-Length: 0\r\n"
                               "\r\n";

constexpr size_t BACKLOG = 128;

condy::Coro<void> sendfile(int out_fd, int in_fd) {
    constexpr size_t CHUNK_SIZE = 8192;

    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        std::fprintf(stderr, "Failed to create pipe: %d\n", errno);
        exit(1);
    }

    while (true) {
        int n = co_await condy::async_splice(in_fd, -1, pipe_fds[1], -1,
                                             CHUNK_SIZE, 0);
        if (n == 0) { // End of file
            co_return;
        }
        if (n < 0) {
            std::fprintf(stderr, "Error during splice operation: %d\n", n);
            exit(1);
        }

        n = co_await condy::async_splice(pipe_fds[0], -1, out_fd, -1,
                                         CHUNK_SIZE, 0);
        if (n < 0) {
            std::fprintf(stderr, "Error during splice operation: %d\n", n);
            exit(1);
        }
    }

    co_await condy::async_close(pipe_fds[0]);
    co_await condy::async_close(pipe_fds[1]);
}

std::string parse_request(const std::string &request) {
    size_t pos = request.find("\r\n");
    if (pos == std::string::npos) {
        return "";
    }

    std::string request_line = request.substr(0, pos);
    size_t method_end = request_line.find(' ');
    if (method_end == std::string::npos) {
        return "";
    }

    size_t path_start = method_end + 1;
    size_t path_end = request_line.find(' ', path_start);
    if (path_end == std::string::npos) {
        return "";
    }

    return request_line.substr(path_start, path_end - path_start);
}

condy::Coro<void> session(int client_fd) {
    char buffer[1024];
    int n = co_await condy::async_recv(client_fd, condy::buffer(buffer), 0);
    if (n < 0) {
        std::fprintf(stderr, "Read error: %d\n", n);
        co_await condy::async_close(client_fd);
        co_return;
    }

    std::string request(buffer, n);

    // Just for simplicity, we only partially parse the HTTP request here.
    std::string path = parse_request(request);
    if (path.empty()) {
        co_await condy::async_send(
            client_fd, condy::buffer(HTTP_400, sizeof(HTTP_400) - 1), 0);
        co_await condy::async_close(client_fd);
        co_return;
    }
    if (path == "/") {
        path = "/index.html";
    }
    std::string file_path = serve_directory + path;
    int file_fd = co_await condy::async_open(file_path.c_str(), O_RDONLY, 0);
    if (file_fd < 0) {
        co_await condy::async_send(
            client_fd, condy::buffer(HTTP_404, sizeof(HTTP_404) - 1), 0);
        co_await condy::async_close(client_fd);
        co_return;
    }

    struct statx statx_buf;
    int r_stat = co_await condy::async_statx(
        file_fd, "", AT_EMPTY_PATH | AT_STATX_SYNC_AS_STAT,
        STATX_SIZE | STATX_MODE, &statx_buf);
    if (r_stat < 0) {
        std::fprintf(stderr, "Failed to statx file: %d\n", r_stat);
        co_await condy::async_send(
            client_fd, condy::buffer(HTTP_500, sizeof(HTTP_500) - 1), 0);
        co_await condy::async_close(file_fd);
        co_await condy::async_close(client_fd);
        co_return;
    }
    if (!S_ISREG(statx_buf.stx_mode)) {
        co_await condy::async_send(
            client_fd, condy::buffer(HTTP_404, sizeof(HTTP_404) - 1), 0);
        co_await condy::async_close(file_fd);
        co_await condy::async_close(client_fd);
        co_return;
    }

    char header_buffer[128];
    int header_length =
        std::snprintf(header_buffer, sizeof(header_buffer), HTTP_200_TEMPLATE,
                      static_cast<int>(statx_buf.stx_size));
    co_await condy::async_send(client_fd,
                               condy::buffer(header_buffer, header_length), 0);
    co_await sendfile(client_fd, file_fd);
    co_await condy::async_close(file_fd);
    co_await condy::async_close(client_fd);
}

condy::Coro<int> co_main(int server_fd) {
    while (true) {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = co_await condy::async_accept(
            server_fd, (struct sockaddr *)&client_addr, &client_len, 0);
        if (client_fd < 0) {
            std::fprintf(stderr, "Failed to accept connection: %d\n",
                         client_fd);
            co_return 1;
        }

        condy::co_spawn(session(client_fd)).detach();
    }
}

void prepare_address(const std::string &host, uint16_t port,
                     sockaddr_in &addr) {
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
}

void usage(const char *prog_name) {
    std::printf(
        "Usage: %s [-h] [-b <address>] [-d <directory>] [-p <port>]\n"
        "  -h               Show this help message\n"
        "  -b <address>     Bind to the specified address (default: 0.0.0.0)\n"
        "  -d <directory>   Serve directory (default: current directory)\n"
        "  -p <port>        Port number to listen on (default: 8080)\n",
        prog_name);
}

int main(int argc, char **argv) noexcept(false) {
    int opt;
    while ((opt = getopt(argc, argv, "hb:d:p:")) != -1) {
        switch (opt) {
        case 'h':
            usage(argv[0]);
            return 0;
        case 'b':
            bind_address = optarg;
            break;
        case 'd':
            serve_directory = optarg;
            break;
        case 'p':
            port = static_cast<uint16_t>(std::stoi(optarg));
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    sockaddr_in server_addr;
    prepare_address(bind_address, port, server_addr);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::perror("Failed to create socket");
        return 1;
    }

    int optval = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval,
                   sizeof(optval)) < 0) {
        std::perror("Failed to set socket options");
        close(server_fd);
        return 1;
    }

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
        0) {
        std::perror("Failed to bind socket");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, BACKLOG) < 0) {
        std::perror("Failed to listen on socket");
        close(server_fd);
        return 1;
    }

    std::printf("Serving HTTP on port %d (http://%s:%d/) ...\n", port,
                bind_address.c_str(), port);

    return condy::sync_wait(co_main(server_fd));
}