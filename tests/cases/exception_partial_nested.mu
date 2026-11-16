unit exception_partial_nested;

extern int printf(const char* fmt, ...);

class Boom : Exception {
    Boom(string m) { super(m); }
}

// #24: the field whose constructor threw never completed, so its user destructor body must not run,
// and neither must the enclosing constructor's body (it never ran either).
int inner_dtor = 0;
int outer_dtor = 0;

struct Inner {
    int* p;
    Inner() {
        p = alloc(1) as int*;
        throw new Boom("inner");
    }
    ~Inner() {
        inner_dtor += 1;
        free(p);
    }
}

struct Outer {
    Inner inner;
    Outer() { }
    ~Outer() { outer_dtor += 1; }
}

// Precision: the members that did finish get their full destructor, in reverse construction order,
// while the one that threw only gets its partial destructor.
int a_ctor = 0;
int a_full = 0;
int b_partial = 0;
int order0 = 0;
int order1 = 0;
int wide_dtor = 0;

struct A {
    int tag;
    int* p;
    A() {
        a_ctor += 1;
        tag = a_ctor;
        p = alloc(1) as int*;
    }
    ~A() {
        if a_full == 0 { order0 = tag; } else { order1 = tag; }
        a_full += 1;
        free(p);
    }
}

struct B {
    int* q;
    B() {
        q = alloc(1) as int*;
        throw new Boom("b");
    }
    ~B() {
        b_partial += 1;
        free(q);
    }
}

struct Wide {
    A a1;
    A a2;
    B b;
    Wide() { }
    ~Wide() { wide_dtor += 1; }
}

// A class owns its value members just like a struct does. Its constructor body is empty, so only the
// member step can throw; the member the throw interrupted still must not run its user destructor.
int ca_full = 0;
int cb_partial = 0;
int class_dtor = 0;

struct CA {
    int* p;
    CA() { p = alloc(1) as int*; }
    ~CA() {
        ca_full += 1;
        free(p);
    }
}

struct CB {
    int* q;
    CB() {
        q = alloc(1) as int*;
        throw new Boom("cb");
    }
    ~CB() {
        cb_partial += 1;
        free(q);
    }
}

class Holder {
    CA a;
    CB b;
    Holder() { }
    ~Holder() { class_dtor += 1; }
}

int main() {
    try { Outer o = Outer(); } catch (e: Boom) { }
    printf("inner=%d outer=%d\n", inner_dtor, outer_dtor);

    try { Wide w = Wide(); } catch (e: Boom) { }
    printf("a_full=%d order=%d,%d b_partial=%d wide_dtor=%d\n", a_full, order0, order1, b_partial,
           wide_dtor);

    try { Holder h = new Holder(); } catch (e: Boom) { }
    printf("ca_full=%d cb_partial=%d class_dtor=%d\n", ca_full, cb_partial, class_dtor);

    bool ok = inner_dtor == 0 and outer_dtor == 0 and a_full == 2 and order0 == 2 and order1 == 1
              and b_partial == 0 and wide_dtor == 0 and ca_full == 1 and cb_partial == 0
              and class_dtor == 0;
    if not ok { return 1; }
    printf("exception_partial_nested ok\n");
    return 0;
}
