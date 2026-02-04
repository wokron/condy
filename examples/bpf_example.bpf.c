#include <vmlinux.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>

char LICENSE[] SEC("license") = "GPL";

struct event {
    u64 timestamp;
    u32 pid;
    char filename[256];
};

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 24);
} events SEC(".maps");

SEC("tracepoint/syscalls/sys_enter_open")
int handle_execve(struct trace_event_raw_sys_enter *ctx) {
    struct event *evt =
        (struct event *)bpf_ringbuf_reserve(&events, sizeof(*evt), 0);
    if (!evt) {
        return 0;
    }
    evt->timestamp = bpf_ktime_get_ns();
    evt->pid = bpf_get_current_pid_tgid() >> 32;
    const char *filename = (const char *)ctx->args[0];
    bpf_probe_read_user_str(&evt->filename, sizeof(evt->filename), filename);
    bpf_ringbuf_submit(evt, 0);
    return 0;
}