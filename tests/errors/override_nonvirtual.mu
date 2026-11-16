// Methods are static-dispatch by default (build 160): overriding one requires the base declaration to be
// `virtual`, otherwise the two methods are unrelated and the derived one would never be reached through
// a base handle.
unit override_nonvirtual;

class A {
    int f() {
        return 1;
    }
}

class B : A {
    override int f() {
        return 2;
    }
}

int main() {
    return 0;
}
