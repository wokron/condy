# Benchmarks

@brief Performance comparison between Condy and other frameworks.

The benchmark source code can be found at [condy-bench](https://github.com/wokron/condy-bench).

Test Environment:
- CPU: AMD Ryzen 9 7945HX with Radeon Graphics × 16
- Storage: SK Hynix HFS001TEJ9X115N (NVMe SSD, 1TB, PCIe 4.0 x4)
- Compiler: clang version 18.1.3
- OS: Linux Mint 22 Cinnamon
- Kernel: 6.8.0-90-generic

## Sequential File Read

We tested the throughput of Condy reading a 2GB file sequentially with different block sizes and task counts, and compared the results with Asio, Aio, and synchronous interfaces.

### Varying Block Size

We fixed the number of concurrent tasks to 16 and gradually increased the block size for each read. The results are shown below. Both axes use logarithmic scales.

![](file_read_block_size.png)

As shown in the figure, when the block size is less than 16KB, regular Condy reading performs best. When the block size exceeds 16KB, Condy Direct IO mode performs best. As the block size increases, the throughput of Condy Direct IO and Aio gradually increases, reaching saturation (~6700 MB/s) at a block size of 256KB. However, before this point, the throughput growth of Aio is slower than that of Condy Direct IO. Using Fixed Fd & Buffer, Condy achieves better performance, but the improvement is not as significant as with Direct IO.

The performance of synchronous reading is close to that of regular Condy at 4KB block size. However, as the block size increases, the throughput of synchronous reading does not change significantly, thus lagging far behind asynchronous methods. Synchronous Direct IO shows a similar growth pattern to Condy Direct IO: when the block size is small, its throughput is inferior to regular synchronous reading, but as the block size increases, its performance gradually surpasses the regular method.

Asio performs the worst, with results inferior to synchronous Direct IO. It is unclear whether this is due to the use of `asio::random_access_file` instead of `asio::stream_file`. However, `asio::stream_file` cannot achieve concurrent read/write, which means it cannot provide file IO throughput by increasing queue depth.

### Varying Number of Tasks

We fixed the block size to 64KB and gradually increased the number of concurrent read tasks. The results are shown below. Both axes use logarithmic scales.

![](file_read_num_tasks.png)

When the concurrency is 4, regular Condy performance is slightly inferior to synchronous reading. But as the concurrency increases, Condy quickly surpasses synchronous reading. Fixed Fd & Buffer brings some performance improvement, but still less than that brought by Direct IO. At low concurrency, Condy Direct IO is slightly inferior to Aio, but as concurrency increases to 32, Condy Direct IO reaches saturation before Aio. Since 64KB is too small for synchronous Direct IO, its performance is worse than regular synchronous in the figure. Asio still performs the worst.

## Random File Read

We tested the throughput of Condy reading a 2GB file randomly with different block sizes and task counts, and compared the results with Asio, Aio, and synchronous interfaces.

### Varying Block Size

We fixed the number of concurrent tasks to 16 and gradually increased the block size for each read. The results are shown below. Both axes use logarithmic scales.

![](file_random_read_block_size.png)

Unlike sequential reading, Direct IO and regular Condy IO have similar performance at 4KB block size. As the block size increases, the throughput of Direct IO grows much faster than that of regular Condy IO. Aio and Condy Direct IO perform similarly at all block sizes. Using Fixed Fd & Buffer does not significantly change Condy's throughput. Synchronous read performance is much weaker than asynchronous methods. Asio performs worse than synchronous methods.

### Varying Number of Tasks

We fixed the block size to 64KB and gradually increased the number of concurrent read tasks. The results are shown below. Both axes use logarithmic scales.

![](file_random_read_num_tasks.png)

The trend of throughput change with increasing number of tasks is similar to that of changing block size. At low concurrency, Direct IO is slightly weaker than regular reading. But as the number of tasks increases, Direct IO shows a more significant performance improvement.

## Echo Server

We tested the throughput of a TCP echo-server implemented with Condy and other methods as the number of connections increases on a single machine. Since the test is conducted locally, it does not fully reflect the performance of a real network card. However, it still allows us to observe the basic overhead brought by different frameworks on network IO.

![](echo_server_num_connections.png)

As the number of connections increases, the throughput first rises and then falls. The later decline is mainly due to contention caused by an increased number of client threads. As shown in the figure, Condy outperforms Asio and Epoll. With file registration, Condy can achieve a small additional performance gain.

## Channel

By varying the number of messages sent and the number of Channels, and measuring the total time taken, we compared the performance of Condy and Asio Channels.

![](channel_number_of_messages.png)

![](channel_task_pairs.png)

As shown in the figures, as the number of messages and concurrent tasks increases, the total time for both Condy and Asio increases linearly. In terms of execution time, Condy achieves a **20x** performance improvement over Asio.

## Coroutine Spawn

By varying the number of coroutines created and measuring the total time taken, we compared the efficiency of Condy and Asio in coroutine creation.

![](spawn_benchmark.png)

As shown in the figure, as the number of coroutines increases, the total time for both Condy and Asio increases linearly. In terms of execution time, Condy achieves a **5x** performance improvement over Asio.

## Coroutine Switch

By repeatedly switching coroutines and measuring the total time taken, we compared the efficiency of Condy and Asio in coroutine switching.

![](post_number_of_messages.png)

As shown in the figure, as the number of switches increases, the total time for both Condy and Asio increases linearly. In terms of execution time, Condy achieves a **15x** performance improvement over Asio.