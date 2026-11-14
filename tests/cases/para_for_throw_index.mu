// para_for_throw_index — the same unwinding rule for the index form: a throwing `operator[]` in the
// middle of the source joins the tasks started so far. Repeated rounds are a smoke test for the
// region leftover (a missed join would leave tasks reading the producer's frame).

unit para_for_throw_index;

use std.cstdio;
use std.thread;

struct Vec {
    public int[8] data;
    public usize n;
    public usize length() { return n; }
}

int operator[](Vec v, usize i) {
    if i == 3 {
        throw new Exception("index failed");
    }
    return v.data[i];
}

int main() {
    int caught = 0;
    for r in 0..20 {
        try {
            Vec v = {};
            v.n = 8;
            for k in 0..8 {
                v.data[k] = k;
            }
            para for x in v {
                thread_sleep(1);
            }
        } catch (e: Exception) {
            caught = caught + 1;
        }
    }
    printf("%d\n", caught);
    return 0;
}
