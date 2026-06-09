# Advanced Coroutine Tutorial Examples

This folder holds the companion code for the `rain_07_coroutines_advanced`
Manim tutorial: a single-threaded `epoll` + coroutine echo server written in
C++23, functional style.

## Build

```bash
make
```

## Example

- `echo_server.cpp`: a single-threaded `epoll` + coroutine echo server. A
  `DetachedTask` (fire-and-forget coroutine) per connection, an `IoAwaiter` that
  registers the fd with epoll and suspends, and an event loop that resumes
  coroutines after `epoll_wait`. One thread serves many concurrent connections —
  a miniature of Rain's `Task` / `EpollReactor` / `EventLoop`.

  **Functional angle**: `std::expected` is used as `Result<T, E>` (with an `Err()`
  helper) so the fallible setup (`make_epoll` / `make_listener`) returns errors as
  explicit values instead of throwing — the very same `Ok` / `Err` style found in
  Rain's `core/result.hpp`. Address construction (`make_addr`) and event
  construction (`make_event`) are pure functions; the coroutine I/O loop is
  inherently side-effecting and stays imperative rather than being forced pure.

## Run

```bash
make run-echo       # then in another shell: nc 127.0.0.1 9200, typed text is echoed back
```

These examples target Linux and use `epoll`, non-blocking sockets, and C++23
coroutines.
