# Async Operation Types

@brief Overview and classification of supported io_uring async operation variants.

io_uring supports a wide range of asynchronous operations and provides many advanced features, resulting in a rich variety of async operation types. Condy offers a detailed and structured classification of io_uring's async operations. This documentation records these categories, which not only help Condy users understand the interfaces, but also provide io_uring users with a basic overview.

> [!NOTE]
> This document is written from the perspective of Condy; some liburing interfaces may not be included.

## Basic Operations

These are fundamental asynchronous operations. Many of them are asynchronous versions of existing system calls. Each operation is submitted once and returns once.

- `condy::async_splice()`
- `condy::async_tee()`
- `condy::async_readv()`
- `condy::async_writev()`
- `condy::async_recvmsg()`
- `condy::async_sendmsg()`
- `condy::async_fsync()`
- `condy::async_nop()`
- `condy::async_timeout()`
- `condy::async_accept()`
- `condy::async_cancel_fd()`
- `condy::async_link_timeout()`
- `condy::async_connect()`
- `condy::async_files_update()`
- `condy::async_fallocate()`
- `condy::async_openat()`
- `condy::async_open()`
- `condy::async_close()`
- `condy::async_read()`
- `condy::async_write()`
- `condy::async_statx()`
- `condy::async_fadvise()`
- `condy::async_fadvise64()`
- `condy::async_madvise()`
- `condy::async_madvise64()`
- `condy::async_send()`
- `condy::async_sendto()`
- `condy::async_recv()`
- `condy::async_openat2()`
- `condy::async_shutdown()`
- `condy::async_unlinkat()`
- `condy::async_unlink()`
- `condy::async_renameat()`
- `condy::async_rename()`
- `condy::async_sync_file_range()`
- `condy::async_mkdirat()`
- `condy::async_mkdir()`
- `condy::async_symlinkat()`
- `condy::async_symlink()`
- `condy::async_linkat()`
- `condy::async_link()`
- `condy::async_getxattr()`
- `condy::async_setxattr()`
- `condy::async_fgetxattr()`
- `condy::async_fsetxattr()`
- `condy::async_socket()`
- `condy::async_cmd_sock()`
- `condy::async_waitid()`
- `condy::async_futex_wake()`
- `condy::async_futex_wait()`
- `condy::async_futex_waitv()`
- `condy::async_fixed_fd_install()`
- `condy::async_fixed_fd_send()`
- `condy::async_ftruncate()`
- `condy::async_bind()`
- `condy::async_listen()`
- `condy::async_epoll_wait()`
- `condy::async_pipe()`

## Multishot Operations

These asynchronous operations are submitted once but may return results multiple times.

- `condy::async_recvmsg_multishot()`
- `condy::async_timeout_multishot()`
- `condy::async_multishot_accept()`
- `condy::async_multishot_accept_direct()`
- `condy::async_read_multishot()`
- `condy::async_recv_multishot()`

## Zero Copy Tx Operations

These asynchronous operations, when successful, return twice: the first return indicates the operation is complete, and the second indicates the buffer is no longer needed.

- `condy::async_sendmsg_zc()`
- `condy::async_send_zc()`
- `condy::async_sendto_zc()`

## Fixed Fd Operations

These asynchronous operations accept the index of a file registered with the kernel, rather than a file descriptor.

- `condy::async_splice()`
- `condy::async_tee()`
- `condy::async_readv()`
- `condy::async_writev()`
- `condy::async_recvmsg()`
- `condy::async_recvmsg_multishot()`
- `condy::async_sendmsg()`
- `condy::async_sendmsg_zc()`
- `condy::async_fsync()`
- `condy::async_accept()`
- `condy::async_accept_direct()`
- `condy::async_multishot_accept()`
- `condy::async_multishot_accept_direct()`
- `condy::async_cancel_fd()`
- `condy::async_connect()`
- `condy::async_fallocate()`
- `condy::async_read()`
- `condy::async_read_multishot()`
- `condy::async_write()`
- `condy::async_fadvise()`
- `condy::async_fadvise64()`
- `condy::async_send()`
- `condy::async_sendto()`
- `condy::async_send_zc()`
- `condy::async_sendto_zc()`
- `condy::async_recv()`
- `condy::async_recv_multishot()`
- `condy::async_shutdown()`
- `condy::async_sync_file_range()`
- `condy::async_cmd_sock()`
- `condy::async_ftruncate()`
- `condy::async_bind()`
- `condy::async_listen()`

## Direct Operations

These asynchronous operations, unlike their original counterparts which return a file descriptor, instead register the file with the kernel and return the index of the file in the kernel's registered file table.

- `condy::async_accept_direct()`
- `condy::async_multishot_accept_direct()`
- `condy::async_openat_direct()`
- `condy::async_open_direct()`
- `condy::async_openat2_direct()`
- `condy::async_socket_direct()`
- `condy::async_pipe_direct()`

## Fixed Buffer Operations

These asynchronous operations additionally accept the index of a memory region registered with the kernel.

- `condy::async_readv()`
- `condy::async_writev()`
- `condy::async_sendmsg_zc()`
- `condy::async_read()`
- `condy::async_write()`
- `condy::async_send_zc()`
- `condy::async_sendto_zc()`

## Provided Buffer Operations

These asynchronous operations accept a set of pre-provided buffers, rather than a specific single buffer. During the async operation, one or more buffers from the provided set are selected for reading or writing.

- `condy::async_recvmsg_multishot()`
- `condy::async_read()`
- `condy::async_read_multishot()`
- `condy::async_send()`
- `condy::async_sendto()`
- `condy::async_recv()`
- `condy::async_recv_multishot()`
