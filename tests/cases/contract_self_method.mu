unit contract_self_method;
use std.cstdio;

// `Self` in a plain interface method is a *parameter* type (SPEC 3.15.6), not a receiver marker
// (that spelling rule belongs to operators). The implementor spells it the same way:
// `int count(Acc c)` satisfies `int count(Self c)`, and the constrained generic can call it.
interface IMixed {
    int count(Self c);
    Self operator+(Self a, Self b);
}

struct Acc {
    public int n;
    int count(Acc c) { return n + c.n; }
    Acc operator+(Acc o) { return Acc(n + o.n); }
}

int getcount<T: IMixed>(T x) {
    return x.count(x);
}

int main() {
    Acc a = { n = 5 };
    Acc b = a + a;
    int c = getcount<Acc>(b);
    if b.n != 10 or c != 20 {
        printf("bad: n=%d count=%d\n", b.n, c);
        return 1;
    }
    printf("self method ok\n");
    return 0;
}
