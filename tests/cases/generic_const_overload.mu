// generic_const_overload.mu — `const` is part of a method's signature after instantiation too: a generic
// class keeps its const/non-const overloads apart (SPEC 3.11).
unit generic_const_overload;

extern int puts(const char* s);

class Box<T> {
    public T v;
    Box(T x) : v(x) { }
    int get() { return 1; }
    int get() const { return 2; }
}

int main() {
    Box<int> m = new Box<int>(1);
    const Box<int> c = new Box<int>(2);
    if m.get() != 1 { puts("non-const overload failed"); return 1; }
    if c.get() != 2 { puts("const overload failed"); return 2; }
    puts("generic const overload ok");
    return 0;
}
