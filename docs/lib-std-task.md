# std.task — the task runtime (M:N scheduler)

> **Related files**
> - This file: [docs/lib-std-task.md](file:///D:/dev/mu_lan/docs/lib-std-task.md)
> - Language contract: [docs/SPEC.md](file:///D:/dev/mu_lan/docs/SPEC.md) §3.17
> - Decision record (design, invariants, phasing): [agents/task-runtime.md](file:///D:/dev/mu_lan/agents/task-runtime.md)
> - Implementation: [libs/std/task.mu](file:///D:/dev/mu_lan/libs/std/task.mu)
> - Platform layer (fibers, TLS, mutex, condvar): [libs/std/platform/windows/std/thread/backend.mu](file:///D:/dev/mu_lan/libs/std/platform/windows/std/thread/backend.mu)

`std.task` is the runtime `para` / `para for` lower to. It is a library: the compiler knows only the
three entry points `muc_para_start`, `muc_para_join` and the region task list, so the engine can change
without touching the language surface.

## Model

| Piece | What it is |
|---|---|
| **Task** | one concurrent unit: a `(void*) => void` body plus a heap context. |
| **Fiber** | the task's stack. Fixed size, **not** growable; taken from a pool and reused. |
| **Carrier** | an OS thread that runs one task at a time and switches fibers in O(1). One per core, plus handoff ones. |
| **Run queue** | one global FIFO protected by the scheduler lock. |
| **Monitor** | a thread that adds a carrier (handoff) when the queue is non-empty and every busy carrier is stalled. |

```
para ──► muc_para_start(body, ctx) ──► [run queue] ──► carrier ──► fiber(body, ctx)
     └─► muc_para_join(handle)  ──► park the joining task, or block a plain thread
```

## API

```c
// Compiler entry points (generated code only).
void* muc_para_start((void*) => void body, void* arg);  // throws TaskLimitError when the cap is hit
void  muc_para_join(void* handle);                      // exactly once per handle
void* muc_para_group_new();
void  muc_para_group_add(void* group, void* handle);
void  muc_para_group_join_free(void* group);

// Settings and introspection.
void  set_max_live_tasks(int n);              // default 10000
void  set_task_stack(usize commit, usize reserve);   // default 64 KiB / 256 KiB
void  set_carrier_limit(int n);               // caps the initial carrier set and handoff growth
int   live_task_count();
int   carrier_count();
```

`TaskLimitError` derives from `Exception`; a refused start is an ordinary exception at the region's join
point (the region releases the failed item, joins what already started, then propagates).

## Guarantees

- **Structured**: every task belongs to a region that joins it. There is no detached task.
- **Join never blocks a carrier**: a joining task parks and the carrier takes the next task, so nested
  `para` cannot starve the queue. `main` / `std.thread` / C threads block a condition variable instead.
- **No stack sharing**: a task's stack is private and survives a park; a finished task's fiber is
  recycled only after the carrier has published `T_DONE`, so a joiner cannot free a task in use.
- **Exception state travels with the task**: the pending-exception slot is saved on switch-out and
  loaded on switch-in, so a task may resume on another carrier (`muc_eh_slot()`).
- **One join per handle**: `muc_para_join` releases the task record; the region joins each handle once.

## Known limitations (0.0.98)

- **Blocking pins a carrier.** `thread_sleep`, a blocking mutex/condvar or a blocking C call does not
  park the task; handoff adds carriers (up to 4x cores) so the rest of the queue keeps moving, but a
  workload that blocks more than 4x cores worth of carriers queues behind them — and if every carrier
  is blocked on a user-level lock whose holder is a parked task, handoff cannot rescue it (task-aware
  mutex/condvar and park points arrive with the IO poller, P4).
- No cancellation, no `task.yield()` / `task.cancelled()` yet; a started task always runs to completion.
- An exception that escapes a task body aborts the process (the B2 thread-boundary rule); exceptions
  caught inside the task behave normally. There is no "rethrow at the join point" yet.
- `errno` / `GetLastError` are **not** saved across a switch (design 6.2 lists them; not implemented).
  A task that parks and resumes on another carrier must not rely on them being preserved.
- A join cycle (A joins B while B joins A) deadlocks; structured lowering cannot express it, but a
  library API could — it is a programming error, not a runtime check.
- Carriers live for the whole process; there is no shutdown (and `set_carrier_limit` after start only
  bounds growth, it never stops a carrier).
- A task stack cannot grow: deep recursion or large locals need a `thread` section (not implemented yet)
  or a larger `set_task_stack`.
- One global run queue with one lock; there is no work stealing yet.

## Cost

A task is a ~72-byte record plus one heap context from the `para for` producer; stacks are pooled
(64 KiB commit, 256 KiB reserve by default). 2000 tasks fan out well inside the process start-up budget
(`tests/cases/para_many_tasks.mu` runs in a few tens of milliseconds end to end on an ordinary
machine), and the carrier set grows only when a task blocks (`tests/cases/para_handoff.mu`).
