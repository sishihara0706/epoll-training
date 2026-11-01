# epoll-training

A hands-on Linux systems programming project for learning `epoll`, non-blocking I/O, TCP stream handling, and backpressure in C.

The program is a newline-delimited TCP echo server. A single `epoll` event loop handles the listening socket, client sockets, a periodic `timerfd`, and a `signalfd` used for shutdown.

[日本語版はこちら](README_ja.md)

## Features

- Multiple concurrent clients on one level-triggered `epoll` loop
- Non-blocking, close-on-exec sockets created with `socket()` and `accept4()`
- Per-client input buffers and FIFO output queues stored through `epoll_event.data.ptr`
- Newline-based message framing across partial and combined TCP reads
- Ordered, partial-write-safe output handling with `EPOLLOUT`
- High/low-water read throttling based on queued output bytes
- Periodic active-client reporting with `timerfd`
- Signal handling inside the event loop with `signalfd`
- Isolated client error handling, including `EPIPE` and connection resets
- Load, slow-reader, and no-reader test programs

## Recent updates

Recent changes since the previous README update:

- Source, headers, and test programs now live in `src/`, `include/`, and `tests/`; build outputs go to `build/`.
- Compile-time log levels control error, informational, and debug output, including client queue dumps.
- Docker support builds and runs the server with GCC 14.
- GitHub Actions builds the server, checks echo responses under load, runs the load test with AddressSanitizer and UndefinedBehaviorSanitizer, and verifies the Docker build.
- Successful pushes to `main` publish a commit-tagged Docker image to GHCR.
- Additional tests cover JSON messages, recovery after backpressure, multiple clients, and isolation of a client that does not read responses.

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
   |     enqueue every complete newline-delimited message
   |     flush the output queue as far as the socket allows
   |
   +-- client / EPOLLOUT
   |     resume flushing the output queue
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
char in_buf[BUFF_SIZE];
size_t in_len;
```

The server keeps incomplete data and searches for `\n`. Every complete line is copied into a new FIFO output-queue node; remaining incomplete bytes stay in the input buffer for the next read. A message that fills the 4096-byte input buffer without a newline is treated as an error and that client is closed.

## Partial writes and backpressure

Non-blocking `write()` may send only part of a response, or fail with `EAGAIN`. Each queued message retains its own write position, while the client tracks the queue and its aggregate size:

```c
struct st_outmsg {
    char buf[BUFF_SIZE];
    size_t len;
    size_t pos;
    struct st_outmsg *next;
};

struct st_outmsg *head;
struct st_outmsg *tail;
size_t queued_bytes;
size_t queued_messages;
int read_paused;
```

For example, `len = 1000` and `pos = 600` on the head message means that 400 bytes remain in that message. Successful writes reduce `queued_bytes`; fully written nodes are removed from the head and freed. This preserves FIFO response order even when several complete lines are waiting.

```text
complete lines available
        |
        v
append messages to FIFO queue
        |
        v
flush from the head as much as possible
        |
        +-- queue empty ---------> EPOLLIN
        |
        +-- reads active and
        |   queue below 64 KiB --> EPOLLIN | EPOLLOUT
        |
        +-- queue reaches 64 KiB -> pause EPOLLIN
                                      |
                                      v
                              flush on EPOLLOUT
                                      |
                         queued bytes <= 32 KiB
                                      |
                                      v
                                 resume EPOLLIN
```

The separate high and low watermarks provide hysteresis, preventing `EPOLLIN` from rapidly toggling near a single threshold. While reads are paused, the kernel receive buffer applies TCP flow control naturally to the sender. `EPOLLOUT` is disabled as soon as the queue becomes empty, avoiding continuous writable notifications.

## Build and run

Requirements are Linux, GCC (or a compatible C compiler), GNU Make, and Python 3 for the test programs.

```bash
make
make run
```

The server listens on all interfaces on TCP port `8080`:

```text
[INFO] listening on 8080
```

To remove the compiled binary:

```bash
make clean
```

The equivalent direct compilation command is:

```bash
mkdir -p build
gcc -Iinclude -DLOG_LEVEL=LOG_INFO -Wall -Wextra -Wpedantic -Og -g src/server.c -o build/server
```

`SO_REUSEADDR` is enabled so the server can usually be restarted without waiting for old TCP state to expire.

## Logging

`LOG_LEVEL` is selected at build time: `LOG_ERROR` (0), `LOG_INFO` (1, default), or `LOG_DEBUG` (2). Each level includes the lower levels. INFO includes startup, shutdown, and active-client reports every 10 seconds; DEBUG adds per-client I/O details and queue dumps. Log messages use stderr; some system-call errors also use `perror()`.

Rebuild when changing the level, because Make does not track changes to compiler options:

```bash
make clean
make LOG_LEVEL=LOG_DEBUG
make run
```

## Sanitizers

```bash
make sanitize
make run-sanitize
```

This builds `build/server-sanitize` with AddressSanitizer and UndefinedBehaviorSanitizer. Run `python3 tests/load_test.py` from another terminal to exercise it. `make clean` removes both normal and sanitizer binaries.

## Docker

Build and run from the repository root (Docker is required):

```bash
docker build -t epoll-server:local .
docker run --rm -p 8080:8080 epoll-server:local
```

The image uses `gcc:14`, runs `make`, and starts `/app/build/server`. The host's port 8080 must be available. Stop the container with `docker stop <container-id>`.

## CI/CD and GHCR

[`.github/workflows/ci-cd.yml`](.github/workflows/ci-cd.yml) runs on pushes and pull requests:

- `build`: compile and upload `build/server` as the `epoll-server` artifact.
- `test`: download that binary, start the server, and run `tests/load_test.py`, checking every echo and applying connection/I/O timeouts.
- `sanitize`: build with ASan/UBSan and run the same load test, checking the server exit status during cleanup.
- `docker-build`: verify that the Docker image builds.
- `publish`: after all four jobs pass, publish to `ghcr.io/sishihara0706/epoll-training` only for pushes to `main`, using `GITHUB_TOKEN` with `packages: write`.

Images are tagged `sha-<full commit SHA>`; the workflow does not create a `latest` tag. To run a published image, replace `<full-commit-sha>` with the SHA of a successful publishing commit (registry access is required):

```bash
docker run --rm -p 8080:8080 ghcr.io/sishihara0706/epoll-training:sha-<full-commit-sha>
```

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
bash tests/clients-connect.sh
```

This opens 100 concurrent `nc` clients and keeps them connected for varying lengths of time.

### Request/response load test

```bash
python3 tests/load_test.py
```

The default configuration creates 100 clients, sends 1000 line-based requests per client, waits for every echo, and reports elapsed time and messages per second.

### Slow-reader test

```bash
python3 tests/slow_reader_test.py
```

One hundred clients send many messages without reading the responses, wait for five seconds, and then close. This makes server-side writes more likely to reach `EAGAIN`.

### No-reader test

```bash
python3 tests/no_reader_test.py
```

One client attempts to send 100,000 messages and never reads the echoed data. This is the strongest backpressure test in the repository: it can block in `sendall()` once TCP flow control reaches the client. Its final sleep keeps the connection available for inspection.

### Additional backpressure tests

- `python3 tests/no_reader_test_real_msg.py`: send newline-delimited JSON without reading responses.
- `python3 tests/backpressure_recovery_test.py`: resume reading after a pause and verify all 5000 responses in order.
- `python3 tests/multi_client_test.py`: verify ordered responses for 20 clients after delaying reads.
- `python3 tests/slow_client_test.py`: check 19 normal clients while one client does not read responses, and report normal-client latency.

These tests are run manually; CI currently runs `tests/load_test.py`.

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

Rebuild with `LOG_LEVEL=LOG_DEBUG` to observe backpressure details. Useful server log lines include:

```text
[DEBUG] flush_output: EAGAIN fd=7 pos=600 len=1000 queued_bytes=400
[DEBUG] handle_client: EPOLLOUT fd=7
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
- Enforcement of the `QUEUE_MAX_BYTES` and `QUEUE_MAX_MESSAGES` hard limits; the constants exist, but the current queue logic only applies high/low-water read throttling
- Draining active clients during graceful shutdown
- Configurable address, port, buffer sizes, and runtime log levels
- Broader automated integration coverage beyond the CI load test
- Connection and throughput statistics
- Comparisons with `select`, `poll`, and edge-triggered `EPOLLET`
- Larger-scale latency and throughput measurements

This repository is intentionally small and direct: its purpose is to make Linux event-driven networking behavior easy to read, run, and observe.
