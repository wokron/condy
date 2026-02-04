#include "bpf_example.skel.h"
#include <bpf/libbpf.h>
#include <condy.hpp>
#include <sys/epoll.h>
#include <unistd.h>

#if !IO_URING_CHECK_VERSION(2, 10) // >= 2.10

struct event {
    uint64_t timestamp;
    uint32_t pid;
    char filename[256];
};

struct Handler {
    static int handle_event(void *ctx, void *data, size_t data_sz) {
        Handler *self = static_cast<Handler *>(ctx);
        return self->handle(data, data_sz);
    }

    int handle(void *data, size_t data_sz) {
        if (data_sz < sizeof(struct event)) {
            std::printf("Data size too small: %zu\n", data_sz);
            return 0;
        }
        struct event *evt = static_cast<struct event *>(data);
        bool ok = event_channel.try_push(*evt);
        if (!ok) {
            std::printf("Event channel full, dropping event\n");
        }
        return 0;
    }

    condy::Channel<struct event> event_channel;
};

condy::Coro<void> event_consumer(condy::Channel<struct event> &channel) {
    char buffer[512];
    while (true) {
        struct event evt = co_await channel.pop();
        if (evt.timestamp == 0) {
            // Channel closed
            break;
        }
        std::snprintf(buffer, sizeof(buffer),
                      "Timestamp: %lu, PID: %u, Filename: %s\n", evt.timestamp,
                      evt.pid, evt.filename);
        buffer[sizeof(buffer) - 1] = '\0';
        co_await condy::async_write(STDOUT_FILENO, condy::buffer(buffer), 0);
    }
}

condy::Coro<int> co_main() {
    struct bpf_example_bpf *skel;
    skel = bpf_example_bpf__open_and_load();
    if (!skel) {
        std::printf("Failed to open and load BPF skeleton\n");
        co_return 1;
    }

    int r;
    r = bpf_example_bpf__attach(skel);
    if (r) {
        std::printf("Failed to attach BPF skeleton: %d\n", r);
        bpf_example_bpf__destroy(skel);
        co_return 1;
    }

    Handler handler{
        .event_channel = condy::Channel<struct event>(1024),
    };

    auto t = condy::co_spawn(event_consumer(handler.event_channel));

    struct ring_buffer *rb =
        ring_buffer__new(bpf_map__fd(skel->maps.events), Handler::handle_event,
                         &handler, nullptr);
    if (!rb) {
        std::printf("Failed to create ring buffer\n");
        handler.event_channel.push_close();
        co_await t;
        bpf_example_bpf__destroy(skel);
        co_return 1;
    }

    int epoll_fd = ring_buffer__epoll_fd(rb);
    if (epoll_fd < 0) {
        std::printf("Failed to get epoll fd from ring buffer\n");
        ring_buffer__free(rb);
        handler.event_channel.push_close();
        co_await t;
        bpf_example_bpf__destroy(skel);
        co_return 1;
    }

    constexpr int MAX_EVENTS = 16;
    struct epoll_event ev[MAX_EVENTS];

    while (true) {
        r = co_await condy::async_epoll_wait(epoll_fd, ev, MAX_EVENTS, 0);
        if (r < 0) {
            std::printf("Error in epoll_wait: %d\n", r);
            break;
        }
        for (int i = 0; i < r; i++) {
            struct ring *ring = ring_buffer__ring(rb, ev[i].data.fd);
            ring__consume(ring);
        }
    }

    handler.event_channel.push_close();
    co_await t;
    ring_buffer__free(rb);
    bpf_example_bpf__destroy(skel);
    co_return 0;
}

int main() { return condy::sync_wait(co_main()); }

#else

int main() {
    std::printf("This example requires io_uring version 2.10 or higher.\n");
    return 1;
}

#endif