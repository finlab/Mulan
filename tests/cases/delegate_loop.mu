// delegate_loop — capture in a `for` body takes one copy per iteration, an empty delegate compares
// with `null`, and a delegate captured before a `para` region can be called inside a section.
unit delegate_loop;

use std.cstdio;

type Get = delegate () => int;

class Acc {
    public int v;
}

int main() {
    // one environment per iteration: every lambda keeps its own value
    Get[3] fs = {};
    for i in 0..3 {
        fs[i] = [i] () => i;
    }
    if fs[0]() != 0 or fs[1]() != 1 or fs[2]() != 2 { printf("per iteration\n"); return 1; }

    // the same loop with `[ref i]` shares one variable — documented, and visible here
    Get[3] refs = {};
    int shared = 0;
    for j in 0..3 {
        shared = j;
        refs[j] = [ref shared] () => shared;
    }
    if refs[0]() != 2 or refs[2]() != 2 { printf("shared ref\n"); return 2; }

    // null comparison
    Get empty = null;
    if not (empty == null) { printf("null eq\n"); return 3; }
    Get notnull = [shared] () => shared;
    if notnull == null { printf("non-null\n"); return 4; }
    if not (notnull != null) { printf("non-null ne\n"); return 5; }

    // a delegate captured before a `para` region may be called inside a section (the environment is
    // immutable for the section; the result goes through a shared class object)
    int base = 5;
    Get g = [base] () => base;
    Acc acc = new Acc();
    para {
        { acc.v = acc.v + g(); }
    }
    if acc.v != 5 { printf("para call\n"); return 6; }

    printf("delegate loop ok\n");
    return 0;
}
