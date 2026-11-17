// const_field.mu — const fields are initialized by the constructor initializer list and are read-only
unit const_field;

extern int puts(const char* s);

struct P {
    const int x;
    int y;
    P(int a, int b) : x(a), y(b) { }
}

struct Q {
    const int v;
    Q(int a) : v(a) { }
}

int main() {
    P p = P(1, 2);
    if p.x != 1 or p.y != 2 { puts("initializer list failed"); return 1; }

    // copy construction copies the const field
    P q = p;
    if q.x != 1 or q.y != 2 { puts("copy failed"); return 1; }

    // move construction (no user move constructor: a bit copy moves the value)
    Q a = Q(5);
    Q b = move a;
    if b.v != 5 { puts("move failed"); return 1; }

    puts("const field ok");
    return 0;
}
