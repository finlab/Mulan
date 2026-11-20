// option_move — a move-only payload: construct by move, move out through `match move`, and the
// zero value of a variant local is `None` (the first alternative).
unit option_move;

use std.option;

extern int puts(const char* s);

nocopy struct V { int[] a; int n; V() {} }

V makev(int n) {
    V v = {};
    v.n = n;
    v.a = new int[n];
    return v;
}

int main() {
    Option<V> none = None;
    if none is Some { puts("none"); return 1; }

    V v = makev(3);
    Option<V> o = Some(move v);
    if o is None { puts("some"); return 2; }
    match move o {
        Some(x): { if x.n != 3 { puts("payload"); return 3; } }
        None: { puts("wrong alt"); return 4; }
    }

    Option<V> p = Some(makev(4));
    Option<V> q = move p;
    match move q {
        Some(x): { if x.n != 4 { puts("moved whole"); return 5; } }
        None: { puts("moved none"); return 6; }
    }

    Option<V> r = Some(makev(5));
    match r {
        Some(ref x): { if x.n != 5 { puts("ref binding"); return 7; } }
        None: { puts("ref none"); return 8; }
    }

    Option<int> zi;
    if zi is Some { puts("zero value"); return 9; }

    puts("option move ok");
    return 0;
}
