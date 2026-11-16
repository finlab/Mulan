// delegate_weak_inline — an immediately-invoked `[weak …]` lambda puts its environment on the stack;
// the weak registration must be dropped when that stack environment dies, or the registry would keep a
// pointer into a dead frame while the captured object is still alive.
unit delegate_weak_inline;

use std.cstdio;

type Get = delegate () => int;

int live = 0;

class Res {
    public int id;
    Res(int i) { id = i; live += 1; }
    ~Res() { live -= 1; }
}

int main() {
    Res o = new Res(6);
    int r = ([weak o] () => o.id)();
    if r != 6 { printf("inline call\n"); return 1; }

    // the stack environment is gone here; `[weak o]` again must not see a stale registration
    int r2 = ([weak o] () => o.id + 1)();
    if r2 != 7 { printf("second inline call\n"); return 2; }

    o = null;
    if live != 0 { printf("hold not released\n"); return 3; }
    printf("delegate weak inline ok\n");
    return 0;
}
