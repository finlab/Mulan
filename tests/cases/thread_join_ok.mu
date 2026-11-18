// The boundary check only fires on a pending exception.
unit thread_join_ok;

use std.thread;

extern int printf(const char* fmt, ...);

int done = 0;

void worker(void* arg) {
    done = 1;
}

int main() {
    Thread t = thread_start(worker, null);
    thread_join(t);
    if done != 1 { printf("worker did not run\n"); return 1; }
    printf("thread_join_ok\n");
    return 0;
}
