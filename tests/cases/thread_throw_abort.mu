// A `std.thread` body is a thread boundary: an exception that escapes it aborts the process, the same
// rule a `para` task body follows. Before this, the pending flag died with the thread and `main`
// carried on as if nothing had happened.
unit thread_throw_abort;

use std.thread;

extern int puts(const char* s);
extern int fflush(void* stream);   // flush(null) flushes every stream, so the lines survive abort

void worker(void* arg) {
    puts("in thread");
    fflush(null);
    throw new Exception("thread boom");
}

int main() {
    puts("before");
    fflush(null);
    Thread t = thread_start(worker, null);
    thread_join(t);
    puts("after join");
    return 0;
}
