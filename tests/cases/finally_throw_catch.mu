unit finally_throw_catch;

extern int printf(const char* fmt, ...);

class A : Exception {
    A() { super("a"); }
}

class B : Exception {
    B() { super("b"); }
}

int main() {
    try {
        try {
            throw new A();
        } catch (e: A) {
            printf("caught-a\n");
            throw new B();
        } finally {
            printf("fin\n");
        }
    } catch (e: B) {
        printf("outer-b\n");
    }
    return 0;
}
