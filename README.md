# epoll-training

A hands-on Linux systems programming project for learning `epoll`, non-blocking I/O, TCP stream handling, and backpressure in C.

The program is a newline-delimited TCP echo server. A single `epoll` event loop handles the listening socket, client sockets, a periodic `timerfd`, and a `signalfd` used for shutdown.

[日本語版はこちら](README_ja.md)

## Features

- Multiple concurrent clients on one level-triggered `epoll` loop
- Non-blocking, close-on-exec sockets created with `socket()` and `accept4()`
- Per-client input and output buffers stored through `epoll_event.data.ptr`
- Newline-based message framing across partial and combined TCP reads
- Partial-write handling with `EPOLLOUT`
- Read-side backpressure while an echo response is still pending
- Periodic active-client reporting with `timerfd`
- Signal handling inside the event loop with `signalfd`
- Isolated client error handling, including `EPIPE` and connection resets
- Load, slow-reader, and no-reader test programs

## Recent updates

The latest changes add behavior that was not covered by the original README:

- Input is accumulated per client and echoed one complete line at a time. TCP read boundaries are no longer treated as message boundaries.
- Multiple complete lines already in the input buffer are processed in order.
- When a response cannot be fully written, the client is monitored for `EPOLLOUT` only. Reading resumes after the pending response has been sent, which bounds application buffering and propagates backpressure to the sender.
- `SIGINT`, `SIGTERM`, and `SIGQUIT` are blocked and consumed through `signalfd`, allowing shutdown to remain part of the `epoll` event loop.
- A `Makefile` now provides standard build, run, and clean targets.
- `no_reader_test.py` was added to deliberately exercise sustained backpressure.

## Event-loop design

Each registered descriptor starts with a small common header:

```c
struct fd_info {
    enum fd_type type;
    int fd;
};
```

`epoll_event.data.ptr` points to this header, or to a client structure whose first member is the same header. The event loop dispatches by descriptor type:

```text
epoll_wait()
   |
   +-- listener / EPOLLIN
   |     accept4() until EAGAIN
   |
   +-- client / EPOLLIN
   |     read into the client's input buffer
   |     extract complete newline-delimited messages
   |     write echoes as far as the socket allows
   |
   +-- client / EPOLLOUT
   |     resume a partial write
   |
   +-- timerfd / EPOLLIN
   |     report the active-client count every 10 seconds
   |
   +-- signalfd / EPOLLIN
         consume a termination signal and shut down
```

The listener is drained until `accept4()` returns `EAGAIN`. Client reads and writes similarly continue until they would block, which is required for efficient non-blocking I/O.

## TCP framing

TCP is a byte stream: one `write()` by the sender does not necessarily become one `read()` by the receiver. For example, these writes:

```text
alpha\n
beta\n
```

may arrive in one read, or either line may be split across several reads.

Each client therefore owns an input buffer:

```c
char in_buf[BUFF_SIZE + 1];
size_t in_len;
```

The server keeps incomplete data, searches for `\n`, and moves one complete line at a time to the output buffer. Remaining bytes stay in the input buffer for the next message. A message that fills the 4096-byte input buffer without a newline is treated as an error and that client is closed.

## Partial writes and backpressure

Non-blocking `write()` may send only part of a response, or fail with `EAGAIN`. The client structure retains the write position across event-loop iterations:

```c
char out_buf[BUFF_SIZE + 2];
size_t out_len;
size_t out_pos;
```

For example, `out_len = 1000` and `out_pos = 600` means that 400 bytes remain.

```text
complete line available
        |
        v
write as much as possible
        |
        +-- response complete --> monitor EPOLLIN
        |
        +-- EAGAIN / partial write
                |
                v
          preserve out_pos
          monitor EPOLLOUT only
                |
                v
          finish the response
          resume EPOLLIN
```

Temporarily disabling `EPOLLIN` is intentional. It prevents the application from continuing to consume requests from a client whose responses cannot be delivered. The kernel receive buffer then applies TCP flow control naturally. `EPOLLOUT` is disabled again as soon as no output is pending, avoiding continuous writable notifications.

## Build and run

Requirements are Linux, GCC (or a compatible C compiler), GNU Make, and Python 3 for the test programs.

```bash
make
make run
```

The server listens on all interfaces on TCP port `8080`:

```text
listening on 8080
```

To remove the compiled binary:

```bash
make clean
```

The equivalent direct compilation command is:

```bash
gcc -Wall -Wextra -Wpedantic -Og -g server.c -o server
```

`SO_REUSEADDR` is enabled so the server can usually be restarted without waiting for old TCP state to expire.

## Shutdown

Stop the server with `Ctrl-C`, `SIGTERM`, or `SIGQUIT`:

```bash
kill -TERM "$(pidof server)"
```

These signals are blocked from their default delivery path and read from a non-blocking `signalfd`. Because that descriptor is registered with `epoll`, signal handling stays synchronous with the rest of the event loop. On receipt, the server closes its listener, timer, signal, and epoll descriptors before exiting.

This is a controlled process shutdown, but it does not drain outstanding client responses before exit.

## Testing

Start the server in one terminal before running these commands in another.

### Manual echo test

```bash
nc 127.0.0.1 8080
```

Each newline-terminated message is echoed exactly once.

### Concurrent connections

```bash
./clients-connect.sh
```

This opens 100 concurrent `nc` clients and keeps them connected for varying lengths of time.

### Request/response load test

```bash
python3 load_test.py
```

The default configuration creates 100 clients, sends 1000 line-based requests per client, waits for every echo, and reports elapsed time and messages per second.

### Slow-reader test

```bash
python3 slow_reader_test.py
```

One hundred clients send many messages without reading the responses, wait for five seconds, and then close. This makes server-side writes more likely to reach `EAGAIN`.

### No-reader test

```bash
python3 no_reader_test.py
```

One client attempts to send 100,000 messages and never reads the echoed data. This is the strongest backpressure test in the repository: it can block in `sendall()` once TCP flow control reaches the client. Its final sleep keeps the connection available for inspection.

The workload values in the Python files can be reduced for shorter runs.

## Observing the server

Inspect TCP queues with:

```bash
ss -tan
ss -tin
```

- `Recv-Q` is data accepted by the kernel but not yet read by the application.
- `Send-Q` is data written by the application but not yet fully delivered through the TCP stack.

Trace the core system calls with:

```bash
strace -p "$(pidof server)" \
  -e trace=epoll_wait,epoll_ctl,accept4,read,write
```

During a backpressure test, useful server log lines include:

```text
write EAGAIN fd=7 pos=600 len=1000
EPOLLOUT fd=7
```

The first line shows a saved partial-write position. The later `EPOLLOUT` event allows the server to continue from that position.

## Error handling

```text
EINTR
    retry the interrupted operation

EAGAIN / EWOULDBLOCK
    preserve state and return to epoll_wait()

EPIPE or ECONNRESET
    close only the affected client

read() == 0
    peer performed an orderly shutdown; close that client
```

`SIGPIPE` is ignored so that a failed socket write becomes an `EPIPE` error instead of terminating the whole process. A client-specific failure decrements the active-client count and leaves the server running.

## Learning topics

- Level-triggered `epoll`
- Non-blocking sockets and readiness-driven I/O
- `accept4()`, `timerfd`, and `signalfd`
- TCP byte-stream framing
- Partial reads and writes
- `EAGAIN`, `EINTR`, `EPIPE`, and connection reset handling
- TCP send/receive queues and flow control
- Per-connection state and file descriptor reuse
- Runtime inspection with `ss` and `strace`

## Development notes

The server was developed incrementally as a Linux systems programming exercise, with behavior checked through load tests, `ss`, and `strace`. AI coding assistants, including Codex and ChatGPT, were used for code review, debugging discussions, edge-case analysis, test design, and documentation support; the implementation was not generated as a one-shot project.

## Current limitations and future work

- Explicit handling for `EPOLLERR`, `EPOLLHUP`, and `EPOLLRDHUP`
- Draining active clients during graceful shutdown
- Configurable address, port, buffer sizes, and logging
- Automated integration tests
- Connection and throughput statistics
- Comparisons with `select`, `poll`, and edge-triggered `EPOLLET`
- Larger-scale latency and throughput measurements

This repository is intentionally small and direct: its purpose is to make Linux event-driven networking behavior easy to read, run, and observe.
