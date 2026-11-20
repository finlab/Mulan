// option_nomove — a copyable payload whose address is part of the value (`nomove`) is accepted:
// the variant is copyable and not movable, so construction and matching copy it.
unit option_nomove;

use std.option;

extern int puts(const char* s);

nomove struct P { int x; }

int main() {
    P p = {};
    p.x = 7;
    Option<P> a = Some(p);
    Option<P> b = a;
    match b {
        Some(x): { if x.x != 7 { puts("payload"); return 1; } }
        None: { puts("none"); return 2; }
    }
    puts("option nomove ok");
    return 0;
}
