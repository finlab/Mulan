// discard_expr_cleanup — an expression statement discards an owned variant or delegate result; the
// payload/environment is released (#156).
unit discard_expr_cleanup;

extern int puts(const char* s);

int live = 0;
int env_live = 0;

class Box {
    public int id;
    Box(int x) { id = x; live += 1; }
    ~Box() { live -= 1; }
}

variant Opt { Some Box; None; }

Opt make_opt() { return Some(new Box(1)); }

class Env {
    public int tag;
    Env(int t) { tag = t; env_live += 1; }
    ~Env() { env_live -= 1; }
}

type Get = delegate () => int;

Get make_cb() {
    Env e = new Env(7);
    Get d = [e] () => e.tag;
    return d;
}

int main() {
    make_opt();
    if live != 0 { puts("variant discarded"); return 1; }
    make_cb();
    if env_live != 0 { puts("delegate discarded"); return 2; }
    puts("discard expr cleanup ok");
    return 0;
}
