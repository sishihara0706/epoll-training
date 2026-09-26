# epoll-training

A hands-on Linux systems programming project for learning `epoll`, non-blocking I/O, TCP socket behavior, and event-driven server design in C.

The project implements a TCP echo server that supports multiple concurrent clients using a single `epoll` event loop. It also includes shell and Python load tests for observing connection handling, TCP buffering, backpressure, `EAGAIN`, and `EPOLLOUT`.

[日本語版はこちら](README_ja.md)

## Goals

The main goal of this project is to understand how Linux network servers behave below the framework level.

Topics explored include:

* `select`, `poll`, and `epoll`
* non-blocking sockets
* `EPOLLIN` and `EPOLLOUT`
* partial reads and partial writes
* `EAGAIN` / `EWOULDBLOCK`
* `EINTR`
* TCP send and receive buffers
* `Recv-Q` / `Send-Q`
* backpressure
* client state management
* periodic processing with `timerfd`
* file descriptor reuse
* `SIGPIPE` / `EPIPE`
* observing system calls with `strace`
* observing TCP state with `ss`

## Features

* TCP echo server written in C
* `epoll`-based I/O multiplexing
* non-blocking sockets
* multiple concurrent clients
* periodic reporting of active client connections with `timerfd`
* `accept4()` with:

  * `SOCK_NONBLOCK`
  * `SOCK_CLOEXEC`
* per-client state stored through `epoll_event.data.ptr`
* partial write handling
* `EPOLLOUT` registration only when a write cannot complete
* retry handling for `EINTR`
* handling of `EAGAIN` / `EWOULDBLOCK`
* client disconnect handling
* `SIGPIPE` / `EPIPE` handling
* shell-based concurrent connection testing
* Python `asyncio` load testing
* slow-reader testing for TCP backpressure

## Architecture

The server follows a typical event-driven design.

```text
socket()
   |
bind()
   |
listen()
   |
epoll_create1()
   |
register listener with epoll
   |
epoll_wait()
   |
   +-- EPOLLIN on listener
   |       |
   |     accept4()
   |       |
   |     allocate client state
   |       |
   |     register client with epoll
   |
   +-- EPOLLIN on client
   |       |
   |     read()
   |       |
   |     prepare echo response
   |       |
   |     write as much as possible
   |       |
   |       +-- all data written
   |       |       |
   |       |     keep EPOLLIN only
   |       |
   |       +-- EAGAIN
   |               |
   |             keep remaining output
   |               |
   |             enable EPOLLOUT
   |
   +-- EPOLLOUT on client
   |       |
   |     resume pending write
   |       |
   |       +-- all data written
   |               |
   |             disable EPOLLOUT
   |
   +-- EPOLLIN on timerfd (every 10 seconds)
           |
         read timer expiration count
           |
         report active client connections
```

## Per-client State

Each client keeps its own output state.

```c
struct st_client {
    struct fd_info base;

    char outbuff[BUFF_SIZE + 2];
    size_t out_len;
    size_t out_pos;
};
```

`out_len` represents the total number of bytes that should be written.

`out_pos` represents how many bytes have already been written.

For example:

```text
out_len = 1000
out_pos = 600
```

means that 400 bytes still need to be sent.

This state is preserved across `epoll_wait()` calls.

## Why EPOLLOUT Is Needed

With a non-blocking socket, `write()` does not guarantee that all requested bytes can be written immediately.

For example:

```text
1000 bytes need to be sent
        |
write() -> 600 bytes
        |
write() -> -1 / EAGAIN
        |
400 bytes remain
        |
enable EPOLLOUT
        |
epoll_wait()
        |
socket becomes writable
        |
continue from byte 600
```

The server normally monitors only:

```text
EPOLLIN
```

When a write cannot complete, it changes the monitored events to:

```text
EPOLLIN | EPOLLOUT
```

After the pending output has been fully written, `EPOLLOUT` is removed again.

This avoids continuously receiving writable events for sockets that have no pending output.

## Build

```bash
gcc -Wall -Wextra -Wpedantic -Og -g server.c -o server
```

## Run

```bash
./server
```

The server listens on TCP port `8080`.

```text
listening on 8080
```

## Periodic Connection Monitoring

The server creates a non-blocking `timerfd` and registers it with the same
`epoll` instance as the listener and client sockets. The timer expires every
10 seconds, allowing periodic work to remain inside the event loop without a
separate thread or signal handler.

On each timer event, the server reports the current number of active client
connections and the number of timer expirations consumed by `read()`:

```text
active clients: 3
timer fired: 1 time(s)
```

The expiration count is normally `1`. It can be greater when the event loop
could not process the timer immediately, because `timerfd` accumulates
expirations until they are read.

## Manual Test

Connect with `nc` from another terminal.

```bash
nc 127.0.0.1 8080
```

Example:

```text
hello
hello
```

Multiple terminals can connect at the same time.

## Concurrent Connection Test

A simple shell test can create many clients concurrently.

Example:

```bash
for i in $(seq 1 100); do
    {
        printf "client-%d\n" "$i"
        sleep 5
    } | nc 127.0.0.1 8080 &
done

wait
```

This was used to verify that the server could accept and process around 100 concurrent connections.

## Python Load Test

The repository also contains Python tests based on `asyncio`.

A typical load test creates many clients and repeatedly sends messages to the echo server.

For example:

```text
100 clients
x
1000 messages
=
100,000 echo requests
```

This makes it easier to test:

* concurrent connections
* repeated reads and writes
* event loop behavior
* disconnect handling

## Slow Reader Test

A slow-reader test is included to intentionally create TCP backpressure.

The client sends many messages to the server but intentionally delays reading the echoed responses.

Conceptually:

```text
client application
      |
      | send many messages
      v
server
      |
      | echo responses
      v
client kernel receive buffer
      |
      | application does not read
      v
Recv-Q grows
      |
      v
TCP flow control
      |
      v
server-side writes become harder
      |
      v
write() -> EAGAIN
      |
      v
wait for EPOLLOUT
```

This test is useful because small echo requests often complete immediately and do not exercise the `EPOLLOUT` path.

## Observing TCP State with ss

Socket state can be inspected with:

```bash
ss -tan
```

Typical output columns are:

```text
State  Recv-Q  Send-Q  Local Address:Port  Peer Address:Port
```

### Recv-Q

`Recv-Q` represents data that has already been received by the kernel but has not yet been consumed by the application with `read()`.

During the slow-reader test, the client-side `Recv-Q` can become large because the client intentionally does not read the echo responses.

### Send-Q

`Send-Q` represents data written by the application that has not yet been fully transmitted or acknowledged through the TCP stack.

For more detailed TCP information:

```bash
ss -tin
```

## Observing System Calls with strace

The event loop can be observed directly with `strace`.

```bash
strace -p $(pidof server) \
  -e trace=epoll_wait,accept4,read,write
```

A typical client message produces something like:

```text
epoll_wait(...)
read(...)
write(...)
read(...) = -1 EAGAIN
epoll_wait(...)
```

This corresponds directly to the server logic:

```text
wait for readiness
        |
read available data
        |
write response
        |
read again
        |
EAGAIN -> no more data currently available
        |
return to epoll_wait
```

New client connections can be observed as:

```text
epoll_wait(...)
accept4(...)
accept4(...) = -1 EAGAIN
epoll_wait(...)
```

The second `accept4()` is intentional.

The server accepts connections until the non-blocking listener returns `EAGAIN`, meaning that the accept queue is currently empty.

## EINTR

System calls can be interrupted by signals.

For example, `strace` may show:

```text
epoll_wait(...) = -1 EINTR
```

The server handles this by retrying the operation rather than treating it as a fatal error.

## File Descriptor Reuse

File descriptor numbers are not permanent client identifiers.

For example:

```text
client A -> fd 5

close(fd 5)

client B -> fd 5
```

Linux commonly reuses low-numbered file descriptors after they are closed.

For this reason, the server stores a pointer to a client structure in:

```c
epoll_event.data.ptr
```

rather than treating the file descriptor number as a persistent client identity.

## TCP Is a Byte Stream

An important result from the load tests was observing that TCP does not preserve application message boundaries.

A sender may write:

```text
message-1\n
message-2\n
message-3\n
```

but the receiver may observe:

```text
read #1 -> "message-1\nmessage"
read #2 -> "-2\nmess"
read #3 -> "age-3\n"
```

A single `write()` does not necessarily correspond to a single `read()`.

This means that production protocols need explicit framing, such as:

* newline-delimited messages
* fixed-length messages
* length-prefixed messages

A future version of this project may add a per-client input buffer and message framing.

## Error Handling

The server handles several important socket conditions.

```text
EINTR
    system call interrupted by a signal
    -> retry

EAGAIN / EWOULDBLOCK
    non-blocking operation cannot proceed now
    -> return to the event loop

EPIPE
    write attempted after peer disconnect
    -> close only that client

read() == 0
    peer performed an orderly shutdown
    -> close the client
```

`SIGPIPE` is ignored so that writing to a disconnected client does not terminate the entire server process.

Instead, the write failure can be handled as `EPIPE`.

## Testing and Debugging Tools

The project uses Linux tools to observe actual runtime behavior rather than relying only on source-level reasoning.

Useful commands include:

```bash
ss -tan
ss -tin
strace -p <pid>
ls /proc/<pid>/fd
```

These tools make it possible to connect the C implementation with actual kernel and TCP behavior.

## Development Process

The core server implementation was written hands-on as part of my Linux systems programming study.

I implemented and iteratively refined the main networking logic myself, including:

* socket setup
* non-blocking I/O
* `epoll` registration and event handling
* `EPOLLIN` / `EPOLLOUT`
* client state management
* partial write handling
* `EAGAIN` / `EINTR`
* disconnect and error handling
* load testing and runtime observation

AI coding tools, including Codex / ChatGPT, were used as development assistants for:

* code review
* debugging support
* discussing Linux API behavior
* identifying edge cases
* suggesting refactoring opportunities
* reviewing error handling
* explaining `epoll`, TCP, and socket behavior
* helping design test scenarios
* assisting with documentation and README authoring

The implementation was not generated as a one-shot project. The server was developed incrementally while testing and inspecting its behavior with tools such as `ss` and `strace`.

## What I Learned

Through this project, I practiced and observed:

* Linux socket programming in C
* I/O multiplexing
* differences between `select`, `poll`, and `epoll`
* event-driven programming
* non-blocking I/O
* `EPOLLIN`
* `EPOLLOUT`
* partial writes
* `EAGAIN`
* `EINTR`
* `SIGPIPE`
* `EPIPE`
* TCP kernel buffers
* `Recv-Q`
* `Send-Q`
* TCP backpressure
* file descriptor reuse
* TCP stream semantics
* load testing
* system call tracing
* runtime socket inspection

## Future Work

Possible next steps include:

* per-client input buffers
* proper newline-based message framing
* `EPOLLERR`
* `EPOLLHUP`
* `EPOLLRDHUP`
* graceful shutdown
* connection statistics
* benchmark result recording
* comparison with `select`
* comparison with `poll`
* Edge Triggered mode with `EPOLLET`
* automated integration tests
* larger-scale concurrent connection testing
* latency and throughput measurement

## Environment

Developed and tested on Linux.

Main Linux APIs used:

```text
socket
setsockopt
bind
listen
accept4
read
write
epoll_create1
epoll_ctl
epoll_wait
close
```

## Purpose

This repository is primarily a learning project for Linux systems programming, TCP networking, and event-driven server design.

The emphasis is not only on making the echo server work, but also on understanding and observing what happens inside Linux when multiple TCP clients are handled concurrently.
