// The rule is about an exception *escaping* the body: catching it inside keeps everything normal.
unit thread_catch_inside;

use std.thread;

extern int printf(const char* fmt, ...);

void worker(void* arg) {
    try {
        throw new Exception("handled");
    } catch (e: Exception) {
        printf("caught inside\n");
    }
}

int main() {
    Thread t = thread_start(worker, null);
    thread_join(t);
    printf("thread_catch_inside\n");
    return 0;
}
