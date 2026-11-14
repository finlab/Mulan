// thread_smoke — threads, mutex and join end to end.
//
// Four workers each take the mutex 1000 times and bump a shared counter; joining before
// printing makes the output deterministic (4000).

unit thread_smoke;

use std.cstdio;
use std.thread;

Mutex g_mutex;
int g_counter = 0;

void worker(void* arg) {
    usize id = arg as usize;
    for i in 0..1000 {
        mutex_lock(g_mutex);
        g_counter = g_counter + 1;
        mutex_unlock(g_mutex);
    }
}

int main() {
    Thread[4] ts;
    for i in 0..4 {
        ts[i] = thread_start(worker, i as usize as void*);
    }
    for i in 0..4 {
        thread_join(ts[i]);
    }
    printf("%d\n", g_counter);
    return 0;
}
