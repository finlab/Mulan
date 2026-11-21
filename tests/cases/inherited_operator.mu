// inherited_operator — a derived class sees its base's operator[] (#179).
unit inherited_operator;

extern int puts(const char* s);

class Base {
    int[10] store;
    isize length() { return 10; }
    ref int operator[](isize i) { return store[i]; }
}
class Derived : Base { }

int main() {
    Derived d = new Derived();
    d[0] = 42;
    if d[0] != 42 { puts("inherited operator"); return 1; }
    puts("inherited operator ok");
    return 0;
}
