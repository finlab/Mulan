unit finally_inner_catch;

extern int printf(const char* fmt, ...);

class A : Exception {
    A() { super("a"); }
}

void risky() {
    throw new A();
}

int main() {
    try {
        try {
            throw new A();
        } finally {
            try {
                risky();
            } catch (e: A) {
                printf("fin-caught\n");
            }
        }
    } catch (e: A) {
        printf("outer\n");
    }
    return 0;
}
