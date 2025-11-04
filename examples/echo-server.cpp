#include <arpa/inet.h>
#include <condy.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

void prepare_address(const std::string &host, uint16_t port,
                     sockaddr_in &addr) {
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
}

condy::Coro<> handle_client(int client_fd) {
    constexpr size_t BUFFER_SIZE = 1024;
    char buffer[BUFFER_SIZE];

    while (true) {
        ssize_t n =
            co_await condy::async_read(client_fd, buffer, BUFFER_SIZE, 0);
        if (n <= 0) {
            if (n < 0) {
                std::fprintf(stderr, "Read error: %s\n", std::strerror(-n));
            }
            break;
        }

        n = co_await condy::async_write(client_fd, buffer, n, 0);
        if (n < 0) {
            std::fprintf(stderr, "Write error: %s\n", std::strerror(-n));
            break;
        }
    }

    co_await condy::async_close(client_fd);
    std::printf("Connection closed, fd:%d\n", client_fd);
}

condy::Coro<> co_main(const std::string &host, uint16_t port) {
    sockaddr_in server_addr;
    prepare_address(host, port, server_addr);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::perror("Failed to create socket");
        co_return;
    }

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
        0) {
        std::perror("Failed to bind socket");
        co_await condy::async_close(server_fd);
        co_return;
    }

    if (listen(server_fd, SOMAXCONN) < 0) {
        std::perror("Failed to listen on socket");
        co_await condy::async_close(server_fd);
        co_return;
    }

    std::printf("Echo server listening on %s:%d\n", host.c_str(), port);

    while (true) {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = co_await condy::async_accept(
            server_fd, (struct sockaddr *)&client_addr, &client_len, 0);
        if (client_fd < 0) {
            std::fprintf(stderr, "Failed to accept connection: %s\n",
                         std::strerror(-client_fd));
            co_return;
        }

        std::printf("Accept connection from %s:%d, fd:%d\n",
                    inet_ntoa(client_addr.sin_addr),
                    ntohs(client_addr.sin_port), client_fd);

        condy::co_spawn(handle_client(client_fd)).detach();
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <host> <port>\n", argv[0]);
        return 1;
    }

    std::string host = argv[1];
    uint16_t port = static_cast<uint16_t>(std::stoi(argv[2]));

    condy::sync_wait(co_main(host, port));
    return 0;
}