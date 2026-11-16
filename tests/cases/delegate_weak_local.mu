// delegate_weak_local — `[weak x]` on a local class handle: same semantics as `[weak this]`, the weak
// registration is dropped with the environment, and the capture is a snapshot of the object at capture
// time (reassigning the variable afterwards does not retarget it).
unit delegate_weak_local;

use std.cstdio;

type Get = delegate () => int;

int live = 0;

class Res {
    public int id;
    Res(int i) { id = i; live += 1; }
    ~Res() { live -= 1; }
}

int via_param(Res o) {
    Get g = [weak o] () => o.id;
    return g();
}

int main() {
    Res o = new Res(4);
    Get g = [weak o] () => o.id;
    if g() != 4 { printf("call while alive\n"); return 1; }

    o = null;
    if live != 0 { printf("hold not released\n"); return 2; }
    if g() != 0 { printf("a dead capture must yield the zero value\n"); return 3; }

    // the capture is a snapshot: `o` now names a new object, `g` still refers to the dead one
    o = new Res(9);
    if live != 1 { printf("replacement live\n"); return 4; }
    if g() != 0 { printf("the capture must not follow the variable\n"); return 5; }
    g = null;

    // a parameter is an ordinary local for capture purposes
    if via_param(o) != 9 { printf("parameter capture\n"); return 6; }
    if live != 1 { printf("parameter capture leaked\n"); return 7; }

    // capture a null handle: the slot is null from the start, so the call is skipped
    Res gone = null;
    Get h = [weak gone] () => 9;
    if h() != 0 { printf("null capture\n"); return 8; }

    o = null;
    if live != 0 { printf("final release\n"); return 9; }
    printf("delegate weak local ok\n");
    return 0;
}
