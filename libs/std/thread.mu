// std.thread — OS threads, mutexes and condition variables (platform-neutral API)
//
// The OS layer lives in std.thread.backend, selected by the loader's platform search path:
//   libs/std/platform/<os>/std/thread/backend.mu
//
// This is the low-level layer the task runtime is built on: it is deliberately C-shaped
// (function values + void* userdata, no captures) and it does not know about `para`. The `para`
// entry points themselves live in `std.task` (the M:N scheduler).

public unit std.thread;

use std.thread.backend;


// Waits forever (same value as INFINITE on Win32; the front-end keeps the portable name).
public const uint WAIT_INFINITE = 0xFFFFFFFF;

// ---------------------------------------------------------------- thread

public struct Thread {
    void* h;
}

// Starts `body(arg)` on a new OS thread. `arg` is passed through untouched: an integer cast
// (`n as void*` with a 64-bit integer), a pointer to alloc'ed memory, or `null`. Its lifetime
// is the caller's responsibility; the thread releases nothing.
public Thread thread_start((void*) => void body, void* arg) {
    void* h = backend_thread_start(body, arg);
    if (h == null) {
        abort();
    }
    Thread t = { h = h };
    return t;
}

public void thread_join(ref Thread t) {
    backend_thread_join(t.h);
    t.h = null;
}

public void thread_detach(ref Thread t) {
    backend_thread_detach(t.h);
    t.h = null;
}

public void thread_sleep(uint ms) { backend_sleep_ms(ms); }

// Monotonic milliseconds (never goes backwards).
public ulong monotonic_ms() { return backend_monotonic_ms(); }

// ---------------------------------------------------------------- mutex

// `s` is opaque storage for the OS lock object, sized for the largest backend: SRWLOCK and
// CONDITION_VARIABLE are 8 bytes, pthread_mutex_t is 40 and pthread_cond_t 48 on 64-bit Linux. Both
// backends accept zeroed memory as an initialised object, which is what lets a Mutex be a plain
// field with no init/destroy call.
public struct Mutex {
    usize[8] s;
}

public void mutex_lock(ref Mutex m) { backend_mutex_lock(&m.s[0]); }
public void mutex_unlock(ref Mutex m) { backend_mutex_unlock(&m.s[0]); }

// ---------------------------------------------------------------- condition variable

public struct CondVar {
    usize[8] s;
}

// Waits until signalled or `ms` elapses (THREAD_WAIT_INFINITE for no timeout). The mutex must
// be held on entry; it is re-acquired before returning. Returns true when signalled.
public bool cond_wait(ref CondVar c, ref Mutex m, uint ms) {
    return backend_cond_wait(&c.s[0], &m.s[0], ms) != 0;
}

public void cond_signal(ref CondVar c) { backend_cond_signal(&c.s[0]); }
public void cond_broadcast(ref CondVar c) { backend_cond_broadcast(&c.s[0]); }

