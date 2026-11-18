// std.thread.backend — pthread/ucontext implementation (Linux only)
//
// Reached through the loader's platform search path:
//   libs/std/platform/<os>/std/thread/backend.mu
// Only this file knows about POSIX; std.thread and std.task stay platform-neutral.
//
// Fibers: a parked task must keep its stack and a switch must be O(1), so each task runs on its own
// stack. Windows has CreateFiber/SwitchToFiber; here the stack is ours (`alloc`) and `swapcontext`
// saves/restores the callee-saved registers plus the signal mask. The carrier's own context lives in
// a pthread key: a fiber switch keeps the OS thread, so "current carrier context" is thread-local.
//
// Mutex/CondVar are zero-initialized blobs (see std.thread): glibc's PTHREAD_MUTEX_INITIALIZER and
// PTHREAD_COND_INITIALIZER are all-zero, so a zeroed blob is a usable default-attributed object
// without an init call — the same contract SRWLOCK/CONDITION_VARIABLE provide on Windows.

public unit std.thread.backend;

use std.exception;

// ---------------------------------------------------------------- libc

extern void abort();
extern int get_nprocs();
extern int clock_gettime(int clock_id, ulong* ts);
extern int nanosleep(ulong* req, ulong* rem);

extern int pthread_create(ulong* t, void* attr, (void*) => void* start, void* arg);
extern int pthread_join(ulong t, void** ret);
extern int pthread_detach(ulong t);
extern int pthread_key_create(uint* key, (void*) => void dtor);
extern void* pthread_getspecific(uint key);
extern int pthread_setspecific(uint key, void* v);
extern int pthread_once(uint* once, () => void init);
extern int pthread_mutex_lock(void* m);
extern int pthread_mutex_unlock(void* m);
extern int pthread_cond_wait(void* c, void* m);
extern int pthread_cond_timedwait(void* c, void* m, ulong* abstime);
extern int pthread_cond_signal(void* c);
extern int pthread_cond_broadcast(void* c);

extern int getcontext(void* uc);
// makecontext's C prototype types the callback as `void (*)(void)`, but it really hands the `argc`
// int arguments to that function. Declaring the true shape here is ABI-identical (both are just a
// code pointer) and keeps the argc/argument contract visible at the call site.
extern void makecontext(void* uc, (uint, uint) => void start, int argc, ...);
extern int swapcontext(void* cur, void* next);

// ---------------------------------------------------------------- thread

public const uint THREAD_WAIT_INFINITE = 0xFFFFFFFF;

struct ThreadCtx {
    (void*) => void body;
    void* arg;
}

void* thread_entry(void* p) {
    ThreadCtx* c = p as ThreadCtx*;
    // An exception that escapes the body has no way out of a thread: the pending slot is
    // thread-local, so it would vanish with the thread. Abort instead, like a `para` task body
    // (SPEC 3.17.7).
    try {
        (*c).body((*c).arg);
    } catch (e: Exception) {
        abort();
    }
    free(c);
    return null;
}

public void* backend_thread_start((void*) => void body, void* arg) {
    ThreadCtx* c = alloc(sizeof(ThreadCtx)) as ThreadCtx*;
    if (c == null) { abort(); }
    (*c).body = body;
    (*c).arg = arg;
    ulong t = 0;
    if (pthread_create(&t, null, thread_entry, c) != 0) {
        // No thread object exists, so the caller decides: `para` turns this into a TaskLimitError,
        // `thread_start` aborts.
        free(c);
        return null;
    }
    return t as void*;
}

public void backend_thread_join(void* h) { pthread_join(h as ulong, null); }
public void backend_thread_detach(void* h) { pthread_detach(h as ulong); }

const int CLOCK_REALTIME = 0;
const int CLOCK_MONOTONIC = 1;

public void backend_sleep_ms(uint ms) {
    ulong[2] ts;
    ts[0] = (ms as ulong) / 1000;
    ts[1] = ((ms as ulong) % 1000) * 1000000;
    nanosleep(&ts[0], null);
}

public ulong backend_monotonic_ms() {
    ulong[2] ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts[0]) != 0) { return 0; }
    return ts[0] * 1000 + ts[1] / 1000000;
}

// ---------------------------------------------------------------- fibers (task stacks)
//
// `ucontext_t` is opaque here: only the three `uc_stack` fields are written, and they sit at fixed
// offsets on every 64-bit Linux ABI (uc_flags, uc_link, then the stack descriptor). The tail is
// oversized padding — glibc's object is 968 bytes on x86-64, and nothing past `ss_size` is touched.

struct UContext {
    ulong flags;      // uc_flags
    void* link;       // uc_link
    void* ss_sp;      // uc_stack.ss_sp
    int ss_flags;     // uc_stack.ss_flags
    usize ss_size;    // uc_stack.ss_size
    ulong[200] rest;  // uc_mcontext / uc_sigmask / fpstate: opaque
}

struct Fiber {
    UContext uc;
    (void*) => void start;
    void* param;
    void* stack;
}

// makecontext only passes `int` arguments, so the fiber record is handed over as two 32-bit halves.
void fiber_entry(uint lo, uint hi) {
    ulong bits = (hi as ulong) << 32;
    bits = bits | (lo as ulong);
    Fiber* f = bits as Fiber*;
    (*f).start((*f).param);
    // std.task's fiber entry never returns: it switches back to the carrier. Reaching here means the
    // task body escaped the scheduler, so there is no context left to resume.
    abort();
}

uint g_cur_key;   // pthread_key_t: context that is running right now (carrier's own or a fiber's)
uint g_cur_once;  // pthread_once_t

// The carrier context is never freed: a carrier thread lives for the whole process and its context
// describes the stack the process would exit on, so there is nothing to release earlier.
void key_dtor(void* p) {}

void cur_key_init() {
    if (pthread_key_create(&g_cur_key, key_dtor) != 0) { abort(); }
}

// The carrier's own context, allocated on first use. It is also the initial value of "running
// context": a carrier starts out running on its thread stack, not on a fiber.
public void* backend_fiber_self() {
    pthread_once(&g_cur_once, cur_key_init);
    void* cur = pthread_getspecific(g_cur_key);
    if (cur == null) {
        UContext* uc = alloc(sizeof(UContext)) as UContext*;
        if (uc == null) { abort(); }
        getcontext(uc);
        pthread_setspecific(g_cur_key, uc);
        return uc;
    }
    return cur;
}

public void* backend_fiber_new((void*) => void start, void* param, usize commit, usize reserve) {
    Fiber* f = alloc(sizeof(Fiber)) as Fiber*;
    void* stack = alloc(reserve);
    if (f == null or stack == null) { abort(); }
    (*f).start = start;
    (*f).param = param;
    (*f).stack = stack;
    getcontext(&(*f).uc);
    (*f).uc.link = null;
    (*f).uc.ss_sp = stack;
    (*f).uc.ss_flags = 0;
    (*f).uc.ss_size = reserve;
    ulong bits = (f as void*) as ulong;
    makecontext(&(*f).uc, fiber_entry, 2, (bits & 0xFFFFFFFF) as uint, (bits >> 32) as uint);
    return f as void*;
}

// Same contract as SwitchToFiber: save the running context and jump to `f`. A Fiber record starts
// with its UContext, so a fiber handle doubles as a context pointer; the carrier's own handle is
// the bare UContext that backend_fiber_self returns.
public void backend_fiber_switch(void* f) {
    pthread_once(&g_cur_once, cur_key_init);
    void* prev = pthread_getspecific(g_cur_key);
    if (prev == null) { prev = backend_fiber_self(); }
    pthread_setspecific(g_cur_key, f);
    swapcontext(prev, f);
    // Back in `prev`: something switched to it, so it is the running context again.
    pthread_setspecific(g_cur_key, prev);
}

public void backend_fiber_free(void* p) {
    Fiber* f = p as Fiber*;
    free((*f).stack);
    free(f);
}

public uint backend_cores() {
    int n = get_nprocs();
    if (n < 1) { return 1; }
    return n as uint;
}

// ---------------------------------------------------------------- TLS

public uint backend_tls_alloc() {
    uint k = 0;
    if (pthread_key_create(&k, key_dtor) != 0) { abort(); }
    return k;
}

public void backend_tls_set(uint k, void* v) { pthread_setspecific(k, v); }
public void* backend_tls_get(uint k) { return pthread_getspecific(k); }

// ---------------------------------------------------------------- mutex / condition variable

public void backend_mutex_lock(void* p) { pthread_mutex_lock(p); }
public void backend_mutex_unlock(void* p) { pthread_mutex_unlock(p); }

// Waits until signalled or `ms` elapses; returns true when signalled. pthread_cond_timedwait takes
// an absolute CLOCK_REALTIME deadline (the default clock of a statically initialised condvar).
public int backend_cond_wait(void* cond, void* lock, uint ms) {
    if (ms == THREAD_WAIT_INFINITE) {
        if (pthread_cond_wait(cond, lock) == 0) { return 1; }
        return 0;
    }
    ulong[2] ts;
    if (clock_gettime(CLOCK_REALTIME, &ts[0]) != 0) { return 0; }
    ulong total = ts[0] * 1000000000 + ts[1] + (ms as ulong) * 1000000;
    ts[0] = total / 1000000000;
    ts[1] = total % 1000000000;
    if (pthread_cond_timedwait(cond, lock, &ts[0]) == 0) { return 1; }
    return 0;
}

public void backend_cond_signal(void* c) { pthread_cond_signal(c); }
public void backend_cond_broadcast(void* c) { pthread_cond_broadcast(c); }
