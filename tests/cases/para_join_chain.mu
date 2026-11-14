// para_join_chain — joins park instead of blocking a carrier, so a nested fan-out still completes
// with a single carrier: the chain A joins B, B joins C, ... finishes bottom-up and every waiter is
// woken onto the one carrier. The carrier count stays 1.

unit para_join_chain;

use std.cstdio;
use std.thread;
use std.task;

class Box {
    public int v;
}

void chain(Box b, int d) {
    if d <= 0 {
        thread_sleep(1);
        b.v = b.v + 1;
        return;
    }
    para {
        { chain(b, d - 1); }
        { chain(b, d - 1); }
    }
}

int main() {
    set_carrier_limit(1);
    Box b = new Box();
    chain(b, 3);
    printf("%d %d\n", b.v, carrier_count());
    return 0;
}
