// delegate_weak_returns — a call skipped by the weak guard must produce a well-formed zero value for
// every return shape: `void`, a class handle and a struct.
unit delegate_weak_returns;

use std.cstdio;

int live = 0;
int ran = 0;

class Res {
    public int id;
    Res(int i) { id = i; live += 1; }
    ~Res() { live -= 1; }
}

struct S {
    public int a;
    public int b;
}

type Do = delegate () => void;
type New = delegate () => Res;
type GetS = delegate () => S;

class Host {
    public Do run;
    public New make;
    public GetS pick;
    int n;
    Host(int x) { n = x; live += 1; }
    ~Host() { live -= 1; }
    void install() {
        run = [weak this] () => { ran += 1; };
        make = [weak this] () => new Res(n);
        pick = [weak this] () => { S s; s.a = n; s.b = n + 1; return s; };
    }
}

int main() {
    Host h = new Host(4);
    h.install();

    Do r = h.run;
    New m = h.make;
    GetS p = h.pick;

    r();
    if ran != 1 { printf("void call\n"); return 1; }
    Res x = m();
    if x.id != 4 { printf("handle call\n"); return 2; }
    x = null;
    S s = p();
    if s.a != 4 or s.b != 5 { printf("struct call\n"); return 3; }

    h = null;
    if live != 0 { printf("host not released\n"); return 4; }

    ran = 0;
    r();
    if ran != 0 { printf("void body ran after death\n"); return 5; }

    Res y = m();
    if y != null { printf("a dead factory must yield a null handle\n"); return 6; }
    if live != 0 { printf("dead factory allocated\n"); return 7; }

    S z = p();
    if z.a != 0 or z.b != 0 { printf("a dead call must yield a zeroed struct\n"); return 8; }

    r = null;
    m = null;
    p = null;
    printf("delegate weak returns ok\n");
    return 0;
}
