unit check_weak_forward;

// a weak field may reference a class declared later; weak fields are assignable and readable

class A {
    weak B b;         // B is declared later
    A() { b = null; }
}

class B { int x; }

int main() {
    A a = new A();
    B b = new B();
    a.b = b;
    B c = a.b;
    c = null;
    b = null;
    a = null;
    return 0;
}
