// return_lifetime — returning a local class handle / variant / delegate transfers one owned
// reference to the caller, and a `match` expression's arm value owns a payload binding (#151-#154).
unit return_lifetime;

extern int puts(const char* s);

int live = 0;
int env_live = 0;

class Box {
    public int id;
    Box(int x) { id = x; live = live + 1; }
    ~Box() { live = live - 1; }
}

Box make_local() {
    Box b = new Box(2);
    return b;
}

variant Opt { Some Box; None; }

Opt pass_through() {
    Opt o = Some(new Box(1));
    return o;
}

Box pick(Opt o) {
    return match o { Some(b): b; None: new Box(99); };
}

class Env {
    public int tag;
    Env(int t) { tag = t; env_live = env_live + 1; }
    ~Env() { env_live = env_live - 1; }
}

type Get = delegate () => int;

Get make_callback() {
    Env e = new Env(7);
    Get d = [e] () => e.tag;
    return d;
}

int main() {
    {
        Box l = make_local();
        if l.id != 2 or live != 1 { puts("class"); return 1; }
    }
    if live != 0 { puts("class release"); return 2; }

    {
        Opt r = pass_through();
        if live != 1 { puts("variant"); return 3; }
        if r is Some(b) {
            if b.id != 1 { puts("payload"); return 4; }
        }
    }
    if live != 0 { puts("variant release"); return 5; }

    {
        Box b = pick(Some(new Box(3)));
        if b.id != 3 or live != 1 { puts("match"); return 6; }
    }
    if live != 0 { puts("match release"); return 7; }

    {
        Get g = make_callback();
        if env_live != 1 { puts("delegate"); return 8; }
        if g() != 7 { puts("callback"); return 9; }
    }
    if env_live != 0 { puts("delegate release"); return 10; }

    puts("return lifetime ok");
    return 0;
}
