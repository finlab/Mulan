// stack_limit.mu — `set_task_stack` really bounds a task's fiber stack: a deep recursion on a 64KB stack
// dies with a stack overflow (SPEC 3.17.6: the task stack is fixed-size, so deep recursion needs a thread
// section or a larger configured stack). The recursion goes through a function value so the optimizer
// cannot turn it into a loop, and the frame buffer escapes so it cannot be eliminated.
unit stack_limit;

use std.task;

class Out { public int t1; }

type Rec = (int) => int;

Rec fp = null;
void* sink = null;

void keep(void* p) { sink = p; }

int deep(int n) {
    byte[512] buf;
    keep(buf);
    if n == 0 { return 0; }
    return fp(n - 1) + 1;
}

int main() {
    fp = deep;
    set_task_stack(65536, 65536);
    Out o = new Out();
    para {
        { o.t1 = fp(20000); }
    }
    // `o.t1` and `sink` stay live so neither the task body nor the frame buffer can be eliminated as
    // dead code (the buffer's address escaping is what makes the frames real).
    if sink == null { return 1; }
    return o.t1 > 0 ? 0 : 2;
}
