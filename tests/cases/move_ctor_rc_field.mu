// move_ctor_rc_field — a user copy/move constructor may write reference-counted fields in its body.
// The destination is zeroed before the constructor runs (SPEC 3.4), so `r = other.r` retains the new
// value and releases null; without it the release read uninitialized memory. A constructor that leaves
// a field alone leaves it null, so the destructor releases null.
unit move_ctor_rc_field;

use std.cstdio;

int live = 0;

class Res {
    public int id;
    Res(int x) { id = x; live = live + 1; }
    ~Res() { live = live - 1; }
}

struct Holder {
    public Res r;
    public int tag;
    Holder(int x) { tag = x; r = new Res(x); }
    Holder(const ref Holder other) { tag = other.tag; r = other.r; }
    Holder(move Holder other) { tag = other.tag; r = other.r; }
    ~Holder() { }
}

struct Sparse {
    public Res r;
    public int tag;
    Sparse(int x) { tag = x; r = new Res(x); }
    Sparse(const ref Sparse other) { tag = other.tag; }   // the field is deliberately left alone
    ~Sparse() { }
}

int main() {
    Holder a = Holder(1);
    if live != 1 { printf("constructed %d\n", live); return 1; }

    Holder b = a;                       // copy constructor, body form
    if b.tag != 1 or live != 1 { printf("copy %d\n", live); return 2; }
    if b.r != a.r { printf("copy shares\n"); return 3; }

    Holder c = move a;                  // move constructor, body form
    if c.tag != 1 { printf("move tag\n"); return 4; }

    { Sparse s = Sparse(7); Sparse t = s; }   // field left alone: the destructor releases null
    if live != 1 { printf("sparse leak %d\n", live); return 5; }

    printf("move ctor rc ok\n");
    return 0;
}
