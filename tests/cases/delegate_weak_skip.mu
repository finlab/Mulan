// delegate_weak_skip — a delegate that outlives its `[weak this]` receiver: the call is skipped (the
// body does not run and the result is the zero value), because the weak registry cleared the slot.
unit delegate_weak_skip;

use std.cstdio;

type Get = delegate () => int;

int live = 0;
int calls = 0;

class Source {
    int n;
    Source(int x) { n = x; live += 1; }
    ~Source() { live -= 1; }
    Get make() { return [weak this] () => { calls += 1; return n; }; }
}

int main() {
    Source s = new Source(5);
    Get g = s.make();
    if g() != 5 { printf("call while alive\n"); return 1; }
    if calls != 1 { printf("live body\n"); return 2; }

    s = null;
    if live != 0 { printf("receiver not destroyed\n"); return 3; }

    calls = 0;
    if g() != 0 { printf("a dead call must yield the zero value\n"); return 4; }
    if calls != 0 { printf("the body ran after the receiver died\n"); return 5; }

    g = null;
    printf("delegate weak skip ok\n");
    return 0;
}
