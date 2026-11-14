// sum_para — a sum value crosses into a `para` section like any other value type: it is read (and
// copied) inside the section, and results leave through a shared class object. Nothing is special
// for sums here (U42 S5).
unit sum_para;

use std.cstdio;

type Option<T> = Some(T) | None;
type Get = delegate () => int;

class Acc {
    public int v;
    public bool seen;
}

int classify(Option<int> o) {
    switch o {
        Some(n): { return n * 2; }
        None: { return -1; }
    }
}

int main() {
    Option<int> a = Some(21);
    Option<int> b = None;
    Acc acc = new Acc();

    para {
        {
            acc.v = classify(a);
            if b is None { acc.seen = true; }
        }
    }

    if acc.v != 42 { printf("sum in section\n"); return 1; }
    if not acc.seen { printf("is in section\n"); return 2; }

    // a sum captured by value in a delegate still behaves like any other value
    Get g = [a] () => classify(a);
    if g() != 42 { printf("sum captured\n"); return 3; }

    printf("sum para ok\n");
    return 0;
}
