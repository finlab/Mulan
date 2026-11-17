// const_struct.mu — `const struct` makes every field and method implicitly const; the type name
// carries no cv, so copy/move and whole-variable reassignment work.
unit const_struct;

extern int puts(const char* s);

const struct P {
    int x;
    int y;
    P(int a, int b) : x(a), y(b) { }
    int sum() { return x + y; }
}

int main() {
    P p = P(1, 2);
    if p.sum() != 3 { puts("sum failed"); return 1; }

    p = P(3, 4);                // whole-variable reassignment
    if p.sum() != 7 { puts("reassignment failed"); return 2; }

    P q = p;                    // copy construction
    if q.sum() != 7 { puts("copy failed"); return 3; }

    puts("const struct ok");
    return 0;
}
