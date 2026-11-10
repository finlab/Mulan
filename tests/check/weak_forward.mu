unit check_weak_forward;

// weak 字段可以引用后声明的 class；weak 字段可赋值、可读取

class A {
    weak B b;         // B 在后面才声明
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
