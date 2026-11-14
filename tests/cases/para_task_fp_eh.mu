// para_task_fp_eh — a task catches an exception and then parks (`thread_sleep`), so it may resume on
// another carrier: the pending-exception slot has to travel with the task, and the floating-point
// state has to survive the fiber switch (the runtime switches with FIBER_FLAG_FLOAT_SWITCH).

unit para_task_fp_eh;

use std.cstdio;
use std.thread;

class Out {
    public double a;
    public double b;
}

double risky(double x) {
    try {
        if x > 0.0 {
            throw new Exception("boom");
        }
    } catch (e: Exception) {
        return x * 2.5;
    }
    return x;
}

int main() {
    Out o = new Out();
    para {
        {
            o.a = risky(2.0);
            thread_sleep(20);
            o.a = o.a + 0.5;
        }
        {
            thread_sleep(20);
            o.b = risky(-1.0) + 1.25;
        }
    }
    printf("%.2f %.2f\n", o.a, o.b);
    return 0;
}
