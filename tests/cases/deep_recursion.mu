// deep_recursion.mu — a deep call stack with a reference-counted object per frame
//
// The recursion covers both the stack (the frames must survive) and the RC cleanup
// (every frame's object must be released when its scope exits).
unit deep_recursion;

use std.cstdio;

// The Windows main thread has a 1 MB stack and a frame below needs about 50 bytes,
// so the stack runs out just above 20000 frames on this platform (measured: 20000
// frames pass, 21000 crash with a stack overflow). 10000 leaves a comfortable margin
// and is still far beyond any hand-written recursion.
const int DEPTH = 10000;

int live = 0;      // objects constructed but not yet destroyed
int peak = 0;      // the high-water mark of `live`

class Frame {
    int n;
    Frame(int v) {
        n = v;
        live += 1;
        if live > peak { peak = live; }
    }
    ~Frame() { live -= 1; }
}

// `acc` is threaded through the whole chain: the root reference must still be usable
// at the deepest level.
int sum(Frame acc, int n) {
    if n == 0 {
        if acc.n != 0 { return -1; }   // the root frame must have survived the chain
        return 0;
    }
    Frame f = new Frame(n);
    return sum(acc, n - 1) + n;
}

int main() {
    Frame root = new Frame(0);
    int r = sum(root, DEPTH);
    // every frame but the root has been released when its scope exited
    printf("sum=%d live=%d peak=%d\n", r, live, peak);
    if r != 50005000 { puts("deep recursion sum failed"); return 1; }
    if live != 1 { puts("deep recursion leak"); return 1; }
    if peak != DEPTH + 1 { puts("deep recursion peak failed"); return 1; }
    puts("deep recursion ok");
    return 0;
}
