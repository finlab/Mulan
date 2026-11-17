// const_method.mu — `const` methods: a const object can call them, overload selection follows C++
unit const_method;

extern int puts(const char* s);

struct P {
    int x;
    int get() const { return x; }
    int which() { return 1; }
    int which() const { return 2; }
}

int main() {
    P a = {7};
    const P b = {9};
    if a.get() != 7 { puts("non-const object calling const method failed"); return 1; }
    if b.get() != 9 { puts("const object calling const method failed"); return 2; }
    if a.which() != 1 { puts("mutable object must prefer the non-const overload"); return 3; }
    if b.which() != 2 { puts("const object must select the const overload"); return 4; }
    puts("const method ok");
    return 0;
}
