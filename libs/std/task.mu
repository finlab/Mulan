// std.task — the M:N task scheduler (SPEC 3.17, P3)
//
// `para` / `para for` lower to `muc_para_start` / `muc_para_join` (the compiler's runtime entry
// points). A task owns a **fiber**: its stack must survive a park, and switching is O(1). Carriers
// are OS threads that run one task at a time; a parked task releases its carrier, and a monitor
// thread adds carriers (handoff) when the run queue is not empty and every carrier is stalled.
//
// Invariants:
//   * One scheduler lock (`S`) protects the run queue, every task state, the waiter lists, the
//     carrier records, the fiber pool and the counters. It is never held across a fiber switch.
//   * `muc_para_start` starts a carrier set on first use; carriers live for the whole process.
//   * One join per handle: `muc_para_join` releases the task record (the region joins exactly once).
//   * Blocking calls (`thread_sleep`, a blocking mutex/condvar) pin a carrier; handoff is the
//     fallback (design 6.4), not park points.

public unit std.task;

use std.thread.backend;

// ---------------------------------------------------------------- public surface

// Live execution units are capped; a refused start throws (SPEC 3.17.7). This is a runtime setting,
// not a per-region parameter: a bigger source is chunked by the user (`chunks(k)`), not by `para`.

public class TaskLimitError : Exception {
public:
    TaskLimitError() {
    }

    TaskLimitError(string msg) {
        super(msg);
    }
}

// ---------------------------------------------------------------- state

const int T_RUNNABLE = 0;
const int T_RUNNING = 1;
const int T_PARKED = 2;
const int T_DONE = 3;

const int MAX_CARRIERS = 256;

const uint WAIT_FOREVER = 0xFFFFFFFF;

struct Task {
    void* fiber;
    (void*) => void body;
    void* arg;
    Task* qnext;    // run queue link
    Task* wnext;    // link while parked on the awaited task
    Task* waiters;  // tasks parked on this one
    int state;
    bool exited;    // the fiber's body returned; only the carrier may publish T_DONE
    ulong eh0;      // saved exception slot (B2 pending flag + object) while the task is not running
    ulong eh1;
}

struct Carrier {
    ulong hb;    // last heartbeat (monotonic ms), written by the carrier while it holds the lock
    bool busy;   // running a task (a stalled busy carrier is what handoff looks for)
}

usize S = 0;    // scheduler lock (SRWLOCK: all-zero is a valid unlocked lock)
usize qcv = 0;  // carriers sleep here when the run queue is empty
usize dcv = 0;  // plain threads (main) sleep here while joining

Task* run_head = null;
Task* run_tail = null;
usize run_n = 0;

Carrier[256] carriers;
int carrier_n = 0;
int carrier_limit = 0;

usize live_tasks = 0;
usize max_live_tasks = 10000;  // default cap (set_max_live_tasks); tasks are cheap, OS threads are not

usize stack_commit = 65536;
usize stack_reserve = 262144;

void** free_fibers = null;
usize free_n = 0;
usize free_cap = 0;

uint tls_next = 0;  // the task currently running on this carrier's fiber
uint tls_self = 0;  // the carrier's own fiber (where a task switches back to)

bool started = false;

// ---------------------------------------------------------------- scheduler primitives

void lock_sched() { backend_mutex_lock(&S); }
void unlock_sched() { backend_mutex_unlock(&S); }

// Both require the scheduler lock. Pushing a task wakes one sleeping carrier: without that signal a
// task can sit in a non-empty queue while every carrier sleeps (lost wakeup), which hangs the join.
void push_run(Task* t) {
    (*t).qnext = null;
    if (run_tail == null) {
        run_head = t;
    } else {
        (*run_tail).qnext = t;
    }
    run_tail = t;
    run_n = run_n + 1;
    backend_cond_signal(&qcv);
}

Task* pop_run() {
    Task* t = run_head;
    if (t == null) {
        return null;
    }
    run_head = (*t).qnext;
    if (run_head == null) {
        run_tail = null;
    }
    (*t).qnext = null;
    run_n = run_n - 1;
    return t;
}

void fiber_release_locked(void* f) {
    if (free_n == free_cap) {
        usize nc = free_cap * 2;
        if (nc == 0) {
            nc = 32;
        }
        void** nf = realloc(free_fibers, nc * sizeof(void*)) as void**;
        if (nf == null) {
            abort();
        }
        free_fibers = nf;
        free_cap = nc;
    }
    free_fibers[free_n] = f;
    free_n = free_n + 1;
}

// ---------------------------------------------------------------- exception slot migration
//
// `muc_eh_pending` is thread-local, but a task may resume on another carrier, so the slot travels
// with the task (design 6.2: save on switch-out, load on switch-in).

extern void* muc_eh_slot();

void eh_load(Task* t) {
    ulong* slot = muc_eh_slot() as ulong*;
    slot[0] = (*t).eh0;
    slot[1] = (*t).eh1;
}

void eh_save(Task* t) {
    ulong* slot = muc_eh_slot() as ulong*;
    (*t).eh0 = slot[0];
    (*t).eh1 = slot[1];
}

// ---------------------------------------------------------------- task body, carrier loop

// The fiber entry. It runs one task per switch-in: the carrier stores the task in `tls_next` before
// switching here, and the fiber switches back when the task finishes (the pool then reuses it).
// The fiber only *marks* the exit: publishing T_DONE is the carrier's job, so that a joiner cannot
// free the task while the carrier is still saving its state (write-after-free).
void fiber_main(void* p) {
    while true {
        Task* t = backend_tls_get(tls_next) as Task*;
        (*t).body((*t).arg);
        lock_sched();
        (*t).exited = true;
        unlock_sched();
        backend_fiber_switch(backend_tls_get(tls_self));
    }
}

void* fiber_acquire() {
    lock_sched();
    if (free_n > 0) {
        free_n = free_n - 1;
        void* f = free_fibers[free_n];
        unlock_sched();
        return f;
    }
    unlock_sched();
    return backend_fiber_new(fiber_main, null, stack_commit, stack_reserve);
}

// Requires the lock: marks the task done, wakes everything waiting for it and recycles its fiber.
void finish_locked(Task* t) {
    (*t).state = T_DONE;
    Task* w = (*t).waiters;
    (*t).waiters = null;
    while w != null {
        Task* nx = (*w).wnext;
        (*w).wnext = null;
        (*w).state = T_RUNNABLE;
        push_run(w);
        w = nx;
    }
    backend_cond_broadcast(&dcv);
    fiber_release_locked((*t).fiber);
}

void carrier_main(void* arg) {
    Carrier* rec = arg as Carrier*;
    backend_tls_set(tls_self, backend_fiber_self());
    backend_tls_set(tls_next, null);
    while true {
        lock_sched();
        Task* t = pop_run();
        if (t == null) {
            (*rec).busy = false;
            (*rec).hb = backend_monotonic_ms();
            backend_cond_wait(&qcv, &S, WAIT_FOREVER);
            unlock_sched();
            continue;
        }
        (*t).state = T_RUNNING;
        (*rec).busy = true;
        (*rec).hb = backend_monotonic_ms();
        backend_tls_set(tls_next, t);
        eh_load(t);
        unlock_sched();
        backend_fiber_switch((*t).fiber);
        // Back on the carrier: the task parked (`exited` false) or its body returned.
        lock_sched();
        eh_save(t);
        (*rec).busy = false;
        (*rec).hb = backend_monotonic_ms();
        if ((*t).exited) {
            finish_locked(t);
        }
        unlock_sched();
    }
}

// ---------------------------------------------------------------- startup, handoff monitor

void start_carrier(int slot) {
    void* h = backend_thread_start(carrier_main, &carriers[slot]);
    if (h == null) {
        abort();
    }
    backend_thread_detach(h);
}

// Called once, outside the lock, after the first `muc_para_start` published the TLS indices and
// flipped `started` (both under the same lock: a concurrent join must never see `started` with an
// unallocated `tls_next`).
void start_runtime() {
    uint cores = backend_cores();
    if (cores < 1) {
        cores = 1;
    }
    if (cores > 64) {
        cores = 64;
    }
    int base = cores as int;

    lock_sched();
    if (carrier_limit <= 0) {
        // Default: one carrier per core, handoff may grow to 4x (still bounded by the table).
        carrier_limit = base * 4;
        if (carrier_limit > MAX_CARRIERS) {
            carrier_limit = MAX_CARRIERS;
        }
    }
    int limit = carrier_limit;
    unlock_sched();

    if (base > limit) {
        base = limit;  // a test / embedder may cap the carrier set
    }
    int i = 0;
    while i < base {
        lock_sched();
        int slot = carrier_n;
        if (slot >= limit) {
            unlock_sched();
            break;
        }
        carrier_n = carrier_n + 1;
        unlock_sched();
        start_carrier(slot);
        i = i + 1;
    }
}

// Handoff (design 6.4): a carrier that stops heartbeating while work is queued is either blocked in
// a C call or busy; the runtime cannot tell the two apart, so it simply adds a carrier, up to 4x the
// cores. Steady state adds none, because every IO path parks instead of blocking.
void monitor_main(void* arg) {
    while true {
        backend_sleep_ms(10);
        bool add = false;
        lock_sched();
        if (run_n > 0 and carrier_n < carrier_limit) {
            ulong now = backend_monotonic_ms();
            int i = 0;
            while i < carrier_n {
                Carrier* c = &carriers[i];
                if ((*c).busy and (now - (*c).hb) > 10) {
                    add = true;
                    (*c).hb = now;  // count one stall once
                }
                i = i + 1;
            }
            if (add) {
                carrier_n = carrier_n + 1;
            }
        }
        unlock_sched();
        if (add) {
            start_carrier(carrier_n - 1);
        }
    }
}

void start_monitor() {
    void* h = backend_thread_start(monitor_main, null);
    if (h == null) {
        abort();
    }
    backend_thread_detach(h);
}

// ---------------------------------------------------------------- compiler entry points

// One task per concurrent unit. The handle is opaque to generated code.
public void* muc_para_start((void*) => void body, void* arg) {
    void* f = fiber_acquire();
    lock_sched();
    if (live_tasks >= max_live_tasks) {
        fiber_release_locked(f);
        unlock_sched();
        throw new TaskLimitError("too many live tasks (limit reached): chunk a large source with chunks(k) or use a plain loop");
    }
    Task* t = alloc(sizeof(Task)) as Task*;
    if (t == null) {
        abort();
    }
    (*t).fiber = f;
    (*t).body = body;
    (*t).arg = arg;
    (*t).qnext = null;
    (*t).wnext = null;
    (*t).waiters = null;
    (*t).state = T_RUNNABLE;
    (*t).exited = false;
    (*t).eh0 = 0;
    (*t).eh1 = 0;
    live_tasks = live_tasks + 1;
    push_run(t);
    bool need_start = not started;
    if (need_start) {
        // The TLS indices and `started` are published in the same critical section: a concurrent
        // `muc_para_join` reads `tls_next` under this lock, so it can never see `started` with an
        // unallocated (0) key and mistake a plain thread for a task.
        tls_next = backend_tls_alloc();
        tls_self = backend_tls_alloc();
        if (tls_next == 0xFFFFFFFF or tls_self == 0xFFFFFFFF) {
            abort();  // TLS_OUT_OF_INDEXES: no way to route joins to the task path
        }
        started = true;
    }
    unlock_sched();
    if (need_start) {
        start_runtime();
        start_monitor();
    }
    return t;
}

// Joins one task. A task parks (its carrier keeps running other work); a plain thread -- `main`, a
// `std.thread` thread, a C thread -- waits on a condition variable.
public void muc_para_join(void* h) {
    Task* t = h as Task*;
    if (t == null) {
        return;  // defensive: generated code always passes a handle returned by muc_para_start
    }
    lock_sched();
    if ((*t).state != T_DONE) {
        // `started` implies the TLS key is allocated (both published under this lock).
        Task* cur = null;
        if (started) {
            cur = backend_tls_get(tls_next) as Task*;
        }
        if (cur == null) {
            while (*t).state != T_DONE {
                backend_cond_wait(&dcv, &S, WAIT_FOREVER);
            }
        } else {
            (*cur).wnext = (*t).waiters;
            (*t).waiters = cur;
            (*cur).state = T_PARKED;
            unlock_sched();
            backend_fiber_switch(backend_tls_get(tls_self));
            lock_sched();
        }
    }
    live_tasks = live_tasks - 1;
    unlock_sched();
    free(t);
}

// ---------------------------------------------------------------- task list
// One region (`para` / `para for`) records the handles of the tasks it started, so the source end
// (or a failed start) can join exactly those. Growable on the heap; the runtime owns the storage.

struct ParaGroup {
    void** hs;
    usize n;
    usize cap;
}

public void* muc_para_group_new() {
    ParaGroup* g = alloc(sizeof(ParaGroup)) as ParaGroup*;
    if (g == null) {
        abort();
    }
    (*g).hs = null;
    (*g).n = 0;
    (*g).cap = 0;
    return g;
}

public void muc_para_group_add(void* p, void* h) {
    ParaGroup* g = p as ParaGroup*;
    if ((*g).n == (*g).cap) {
        usize nc = (*g).cap * 2;
        if (nc == 0) {
            nc = 16;
        }
        void** nh = realloc((*g).hs, nc * sizeof(void*)) as void**;
        if (nh == null) {
            abort();
        }
        (*g).hs = nh;
        (*g).cap = nc;
    }
    (*g).hs[(*g).n] = h;
    (*g).n = (*g).n + 1;
}

// Joins every started task (in start order) and releases the list. Used both at the source end and
// on a failed start, so a region never leaves a task running behind an unwound producer frame.
public void muc_para_group_join_free(void* p) {
    ParaGroup* g = p as ParaGroup*;
    usize i = 0;
    while i < (*g).n {
        muc_para_join((*g).hs[i]);
        i = i + 1;
    }
    free((*g).hs);
    free(g);
}

// ---------------------------------------------------------------- runtime settings

public void set_max_live_tasks(int n) {
    lock_sched();
    if (n > 0) {
        max_live_tasks = n as usize;
    }
    unlock_sched();
}

public void set_task_stack(usize commit, usize reserve) {
    lock_sched();
    if (commit > 0) {
        stack_commit = commit;
    }
    if (reserve >= commit) {
        stack_reserve = reserve;
    }
    unlock_sched();
}

// Caps the carrier set. Call it before the first `para` to also cap the initial set; afterwards it
// only bounds handoff growth (started carriers are never stopped).
public void set_carrier_limit(int n) {
    lock_sched();
    if (n > 0) {
        if (n > MAX_CARRIERS) {
            n = MAX_CARRIERS;
        }
        carrier_limit = n;
    }
    unlock_sched();
}

public int live_task_count() {
    lock_sched();
    int n = live_tasks as int;
    unlock_sched();
    return n;
}

public int carrier_count() {
    lock_sched();
    int n = carrier_n;
    unlock_sched();
    return n;
}
