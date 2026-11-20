// A destructor-bearing struct may be constructed as a call argument (SPEC 3.4): the temporary is
// built for the parameter, and nothing leaks on the way in or out.
unit dtor_temp_arg;

extern int puts(const char* s);

int live = 0;

struct R {
    int* p;
    R(int v) {
        p = alloc(4) as int*;
        p[0] = v;
        live = live + 1;
    }
    R(const ref R o) {
        p = alloc(4) as int*;
        p[0] = o.p[0];
        live = live + 1;
    }
    R(move R o) {
        p = o.p;
        o.p = null;
    }
    ~R() {
        if p != null {
            free(p);
            live = live - 1;
        }
    }
    int value() { return p[0]; }
}

struct Holder {
    public int k;
    Holder(int v) { k = v; }
    int eat(R r) { return r.value() + k; }
}

void take(R r) { }

int main() {
    take(R(1));
    if live != 0 { puts("free function argument leaked"); return 1; }

    Holder h = Holder(10);
    if h.eat(R(2)) != 12 { puts("method argument"); return 2; }
    if live != 0 { puts("method argument leaked"); return 3; }

    puts("dtor temp arg ok");
    return 0;
}
