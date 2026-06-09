# Thread Tutorial Examples

This folder holds the companion code for the `rain_02_thread` Manim tutorial:
a single general-purpose job thread pool written in C++23, functional style.

## Build

```bash
make
```

## Example

- `thread_pool_futures.cpp`: a general-purpose job pool. `submit` returns
  `std::future<R>` (via `packaged_task`) so the caller can retrieve results, and
  the destructor drains gracefully — it runs the queued jobs before joining. This
  mirrors the ThreadPool that backs `spawn_blocking` in the Rain runtime.

  **Functional angle**: the pool itself is an imperative "dirty shell" (queue +
  mutex + condition variable), but it exposes a functional interface. `main`
  drives the whole flow declaratively with a pure `square` and a ranges pipeline:
  `iota | transform | ranges::to` builds the future vector, `views::enumerate`
  pulls results, and `ranges::fold_left` sums them — no hand-written loops, no
  mutable accumulators scattered around.

## Run

```bash
make run-pool-futures
```

It dispatches 8 square jobs to the pool to run concurrently, then retrieves the
results in order and sums them.

These examples use C++23 standard threading facilities and require pthread
support at link time.
