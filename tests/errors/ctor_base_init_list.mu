// A base-class initializer in the list names the base constructor, not a field; the language does not have
// that form yet (use `super(...)`, which cannot be combined with an initializer list yet).
unit ctor_base_init_list;

const class Base {
    public int v;
    Base(int x) : v(x) { }
}

const class Derived : Base {
    public int w;
    Derived(int x) : Base(x), w(x) { }
}

int main() { return 0; }
