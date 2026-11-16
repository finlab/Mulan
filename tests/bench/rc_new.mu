unit bench_rc_new;

// Reference-counting benchmark for `--single-thread`: each iteration allocates a class instance and
// hands it to a global strong reference, so the previous instance is released (and destroyed) in the
// same step. The handle escaping into a global keeps the optimizer from proving the allocation dead.
// Run it in both modes and compare:
//
//   muc build tests/bench/rc_new.mu --run
//   muc build tests/bench/rc_new.mu --single-thread --run
//
// `clock()` (CRT, CLOCKS_PER_SEC = 1000 on both MSVC and MinGW) is used instead of std.thread's
// monotonic clock, which `--single-thread` rejects by design.

use std.cstdio;

extern long clock();

class Node {
    int v;
    Node(int x) { v = x; }
}

Node last;

int main() {
    int n = 20000000;
    long acc = 0;
    long t0 = clock();
    for i in 0..n {
        Node p = new Node(i);
        last = p;       // one retain, one release of the previous instance
        acc = acc + p.v;
    }
    long t1 = clock();
    last = null;
    long ms = t1 - t0;
    long tenths = (ms * 10000000) / n;  // ns per object, in tenths
    printf("%ld ms / %d objects = %ld.%ld ns/object (checksum %ld)\n",
           ms, n, tenths / 10, tenths % 10, acc % 997);
    return 0;
}

