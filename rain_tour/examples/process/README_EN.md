# Process Tutorial Examples

This folder holds the companion code for the `rain_01_process` Manim tutorial:
a single crash-tolerant supervisor written in C++23, functional style.

## Build

```bash
make
```

## Example

- `crash_supervisor.cpp`: a crash-tolerant supervisor. The parent forks several
  workers; one deliberately segfaults (null deref) on its 3rd heartbeat. The
  supervisor detects the death in its `waitpid` loop (`WIFSIGNALED`) and respawns
  a healthy worker in its place while the others keep running untouched. This
  demonstrates the one thing only processes give you — a **crash boundary** — the
  basis of Nginx master/worker, systemd, and Erlang/OTP supervisors.

  **Functional angle**: "interpret the wait status" (`classify`) and "locate the
  slot" (`find_slot`) are pulled out as pure functions — same input, same output,
  unit-testable without spawning anything. The side effects (`fork` / `waitpid` /
  printing) stay at the edge in `main`: a "pure core, dirty shell" split. Whether
  a worker crashed is expressed with `std::optional`, not a magic `-1`.

## Run

```bash
make run-supervisor
```

It prints interleaved worker heartbeats: you can watch one worker segfault, get
replaced by a fresh worker with a new PID, while the other workers' heartbeats
never skip a beat — that is the crash boundary of process isolation.

Conversely, this explains Rain's trade-off: Rain runs a per-core EventLoop on
threads + coroutines, giving up this crash isolation in exchange for a shared
address space and zero-IPC-cost performance.

> Implementation note: SIGALRM is installed with `sigaction` and **without**
> `SA_RESTART`, so it can interrupt the blocking `waitpid` (returning `EINTR`)
> and shut down cleanly. `std::signal` on glibc defaults to `SA_RESTART`, which
> auto-restarts `waitpid` and hangs the loop forever — a real-world trap.

Targets Linux; uses POSIX APIs such as `fork`, `waitpid`, `sigaction`, and
`setrlimit`.
