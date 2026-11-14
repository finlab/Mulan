// para_handoff — a carrier that blocks (here in `thread_sleep`) is invisible to the scheduler, so
// the monitor adds carriers while work is queued (design 6.4). The source is sized from the initial
// carrier set, so the queue cannot drain in one round: the carrier count must grow.

unit para_handoff;

use std.cstdio;
use std.thread;
use std.task;

int main() {
    int[] v = new int[8];
    para for i in 0..8 {
        v[i] = i;
    }
    int base = carrier_count();
    int n = base * 4 + 8;
    para for i in 0..n {
        thread_sleep(60);
    }
    int after = carrier_count();
    if after > base {
        printf("grew\n");
    } else {
        printf("flat\n");
    }
    v = null;
    return 0;
}
