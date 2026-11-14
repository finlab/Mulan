// std.thread.backend — Win32 implementation (Windows only)
//
// Reached through the loader's platform search path:
//   libs/std/platform/<os>/std/thread/backend.mu
// Only this file knows about Win32; std.thread is platform-neutral.

public unit std.thread.backend;

// ---------------------------------------------------------------- thread

public const uint THREAD_WAIT_INFINITE = 0xFFFFFFFF;

extern void* CreateThread(void* sa, usize stack, (void*) => uint start, void* arg, uint flags, uint* id);
extern uint WaitForSingleObject(void* h, uint ms);
extern int CloseHandle(void* h);
extern void Sleep(uint ms);
extern ulong GetTickCount64();
extern void abort();

struct ThreadCtx {
    (void*) => void body;
    void* arg;
}

uint thread_trampoline(void* p) {
    ThreadCtx* c = p as ThreadCtx*;
    (*c).body((*c).arg);
    free(c);
    return 0;
}

public void* backend_thread_start((void*) => void body, void* arg) {
    ThreadCtx* c = alloc(sizeof(ThreadCtx)) as ThreadCtx*;
    if (c == null) { abort(); }
    (*c).body = body;
    (*c).arg = arg;
    void* h = CreateThread(null, 0, thread_trampoline, c, 0, null);
    if (h == null) {
        // No thread object exists, so the caller decides: `para` turns this into a TaskLimitError,
        // `thread_start` aborts.
        free(c);
        return null;
    }
    return h;
}

public void backend_thread_join(void* h) {
    WaitForSingleObject(h, THREAD_WAIT_INFINITE);
    CloseHandle(h);
}

public void backend_thread_detach(void* h) { CloseHandle(h); }
public void backend_sleep_ms(uint ms) { Sleep(ms); }
public ulong backend_monotonic_ms() { return GetTickCount64(); }

// ---------------------------------------------------------------- fibers (task stacks)
//
// The task scheduler runs each task on its own fiber: a parked task keeps its stack, and switching
// is O(1). Task stacks are fixed (SPEC 3.17.6); a reuse pool makes spawning cheap.
//
// FIBER_FLAG_FLOAT_SWITCH (0x1) is passed on both sides: a task body may use float/double, and
// without the flag the OS gives no guarantee that the FP state survives a switch.

const uint FIBER_FLOAT = 0x1;

extern void* ConvertThreadToFiberEx(void* param, uint flags);
extern void* CreateFiberEx(usize commit, usize reserve, uint flags, (void*) => void start, void* param);
extern void SwitchToFiber(void* fiber);
extern void DeleteFiber(void* fiber);
extern uint GetActiveProcessorCount(ushort group);

// Per-OS-thread storage. A fiber switch keeps the thread, so this is exactly "carrier-local".
extern uint TlsAlloc();
extern int TlsSetValue(uint idx, void* v);
extern void* TlsGetValue(uint idx);

public void* backend_fiber_self() { return ConvertThreadToFiberEx(null, FIBER_FLOAT); }

public void* backend_fiber_new((void*) => void start, void* param, usize commit, usize reserve) {
    return CreateFiberEx(commit, reserve, FIBER_FLOAT, start, param);
}

public void backend_fiber_switch(void* f) { SwitchToFiber(f); }
public void backend_fiber_free(void* f) { DeleteFiber(f); }
public uint backend_cores() { return GetActiveProcessorCount(0xFFFF); }
public uint backend_tls_alloc() { return TlsAlloc(); }
public void backend_tls_set(uint k, void* v) { TlsSetValue(k, v); }
public void* backend_tls_get(uint k) { return TlsGetValue(k); }

// ---------------------------------------------------------------- mutex / condition variable
//
// SRWLOCK and CONDITION_VARIABLE are pointer-sized and zero-initialized, so a zeroed
// Mutex/CondVar is immediately usable (no init/destroy call).

extern void AcquireSRWLockExclusive(void* p);
extern void ReleaseSRWLockExclusive(void* p);
extern int SleepConditionVariableSRW(void* cond, void* lock, uint ms, uint flags);
extern void WakeConditionVariable(void* cond);
extern void WakeAllConditionVariable(void* cond);

public void backend_mutex_lock(void* p) { AcquireSRWLockExclusive(p); }
public void backend_mutex_unlock(void* p) { ReleaseSRWLockExclusive(p); }
public int backend_cond_wait(void* cond, void* lock, uint ms) {
    return SleepConditionVariableSRW(cond, lock, ms, 0);
}
public void backend_cond_signal(void* cond) { WakeConditionVariable(cond); }
public void backend_cond_broadcast(void* cond) { WakeAllConditionVariable(cond); }
