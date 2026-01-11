#include <arpa/inet.h>
#include <condy.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

constexpr size_t BACKLOG = 128;
constexpr size_t MAX_CONNECTIONS = 1024;
constexpr size_t MAX_MESSAGE_LEN = 2048;

condy::Coro<void> session(int client_fd) {
    char buffer[MAX_MESSAGE_LEN];

    while (true) {
        int n = co_await condy::async_recv(condy::fixed(client_fd),
                                           condy::buffer(buffer), 0);
        if (n <= 0) {
            if (n < 0) {
                std::fprintf(stderr, "Read error: %d\n", n);
            }
            break;
        }

        n = co_await condy::async_send(condy::fixed(client_fd),
                                       condy::buffer(buffer, n), 0);
        if (n < 0) {
            std::fprintf(stderr, "Write error: %d\n", n);
            break;
        }
    }

    co_await condy::async_close(condy::fixed(client_fd));
}

condy::Coro<int> co_main(int server_fd) {
    while (true) {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = co_await condy::async_accept_direct(
            server_fd, (struct sockaddr *)&client_addr, &client_len, 0,
            CONDY_FILE_INDEX_ALLOC);
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

int main(int argc, char **argv) noexcept(false) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <host> <port>\n", argv[0]);
        return 1;
    }

    std::string host = argv[1];
    uint16_t port = static_cast<uint16_t>(std::stoi(argv[2]));

    sockaddr_in server_addr;
    prepare_address(host, port, server_addr);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::perror("Failed to create socket");
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

    std::printf("Echo server listening on %s:%d\n", host.c_str(), port);

    condy::Runtime runtime(condy::RuntimeOptions().sq_size(MAX_CONNECTIONS));

    runtime.fd_table().init(MAX_CONNECTIONS);

    return condy::sync_wait(runtime, co_main(server_fd));
}