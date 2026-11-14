// para_for_cursor — the streaming form of `para for` (SPEC 3.17.3): the producer pulls the cursor
// once per item (a blocking `next()` is where backpressure lives) and every item becomes its own
// task; the region joins when the source ends.

unit para_for_cursor;

use std.cstdio;

class Out {
    public int[6] r;
}

struct Counter {
    public int i;
    public int n;
    public bool next() {
        i = i + 1;
        return i <= n;
    }
    public int current() { return i; }
}

struct Source {
    public int n;
    public Counter iterator() {
        Counter c;
        c.i = 0;
        c.n = n;
        return c;
    }
}

int sq(int x) { return x * x; }

int main() {
    Source s = { n = 6 };
    Out o = new Out();
    para for v in s {
        o.r[v - 1] = sq(v);
    }
    int sum = 0;
    for k in 0..6 {
        sum = sum + o.r[k];
    }
    printf("%d\n", sum);
    return 0;
}
