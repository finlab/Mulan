// mem_take — `std.mem.take` / `std.mem.replace`: moving a value out of a borrow.
//
// The language mechanism is `move` on a `ref` parameter: it transfers the *referent*, so the caller's
// slot becomes the type's zero value — the same destructible empty shell `move x` leaves on a local.
// The two library functions are ordinary code on top of it, so the "how does the new value come in"
// part is the language's own assignment rules.
unit mem_take;

use std.mem;
use std.cstdio;

int allocs = 0;
int frees = 0;

struct Buf {
    int* p;
    Buf() { p = null; }
    Buf(int n) { p = alloc(8) as int*; p[0] = n; allocs = allocs + 1; }
    ~Buf() {
        if (p != null) { frees = frees + 1; free(p); p = null; }
    }
}

int main() {
    {
        Buf a = Buf(1);
        Buf old = std.mem.replace<Buf>(a, Buf(2));   // move Buf(2) in, hand the old value out
        printf("%d %d\n", old.p[0], a.p[0]);

        Buf out = std.mem.take<Buf>(a);              // a is empty afterwards
        printf("%d\n", out.p[0]);
        a = Buf(3);                                  // assigning re-arms the slot
        printf("%d\n", a.p[0]);
    }
    // five allocations, five releases: nothing was duplicated and no emptied slot ran a destructor
    printf("%d %d\n", allocs, frees);

    // an element of a managed sequence is a place too
    int[] arr = new int[2];
    arr[0] = 5;
    int prev = std.mem.replace<int>(arr[0], 9);
    printf("%d %d\n", prev, arr[0]);

    // the container pattern: swap_remove — replace the removed slot with the last element
    string[] names = new string[3];
    names[0] = string("a");
    names[1] = string("b");
    names[2] = string("c");
    string gone = std.mem.replace<string>(names[1], names[2]);
    printf("%s %s\n", gone.cstr(), names[1].cstr());
    return 0;
}
