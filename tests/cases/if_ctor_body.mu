// if_ctor_body.mu — a constructor body is a function body: a statement-list `#if` works there too
// (SPEC 9.2), including the leading-statement position.
unit if_ctor_body;

extern int puts(const char* s);

const bool FAST = false;

class Cfg {
    public int mode;
    Cfg(int raw) {
#if FAST {
        mode = raw * 10;
}
#else {
        mode = raw;
}
    }
}

struct Pair {
    public int a;
    public int b;
    Pair(int v) {
#if FAST {
        a = v;
}
#else {
        a = v;
        b = v + 1;
}
    }
}

int main() {
    Cfg c = new Cfg(3);
    if c.mode != 3 { puts("ctor-body #if failed"); return 1; }
    Pair p = Pair(7);
    if p.a != 7 or p.b != 8 { puts("struct ctor-body #if failed"); return 2; }
    puts("ctor body if ok");
    return 0;
}
