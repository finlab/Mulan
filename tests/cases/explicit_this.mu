unit explicit_this;
extern int puts(const char* s);
int live = 0;

class A {
    int v;
    A(int x) { this.v = x; live += 1; }
    ~A() { this.v = 0; live -= 1; }
    int get() { return this.v; }
    A self() { return this; }
    int twice() { return this.get() * 2; }
}

packed struct P {
    int x;
    byte tag;
    int y;
    P(int a, int b) { this.x = a; this.y = b; this.tag = 1; }
    int sum() { return this.x + this.y; }
    void bump() { this.x += 1; }
    int take(P o) { return o.x + o.y; }
    int pass_self() { return this.take(this); }
    P dup() { return this; }
}

struct Holder {
    A a;
    Holder(A x) { this.a = x; }
    ~Holder() { this.a = null; }
    int peek() { return this.a.get(); }
}

class Box {
    P p;
    Box() { this.p = P(4, 5); }
    int sum() { return this.p.sum(); }
}

int main() {
    P p = P(1, 2);
    if p.sum() != 3 { puts("struct implicit failed"); return 1; }
    if p.pass_self() != 3 { puts("struct this argument failed"); return 1; }
    p.bump();
    if p.x != 2 { puts("struct this write failed"); return 1; }

    P q = p.dup();
    q.x = 100;
    if p.x != 2 { puts("struct this return should copy"); return 1; }
    if q.sum() != 102 { puts("struct this copy value failed"); return 1; }

    A a = new A(7);
    if a.get() != 7 { puts("class this field failed"); return 1; }
    if a.twice() != 14 { puts("class this call failed"); return 1; }
    if a.self().get() != 7 { puts("class this return failed"); return 1; }

    {
        Holder h = Holder(a);
        if h.peek() != 7 { puts("struct this rc field failed"); return 1; }
    }

    Box b = new Box();
    if b.sum() != 9 { puts("class struct field this failed"); return 1; }

    del a;
    if live != 0 { puts("this dtor cleanup failed"); return 1; }
    puts("explicit this ok");
    return 0;
}
