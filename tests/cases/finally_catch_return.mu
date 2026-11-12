unit finally_catch_return;

extern int printf(const char* fmt, ...);

class E : Exception {
    E() { super("e"); }
}

int f() {
    try {
        throw new E();
    } catch (e: E) {
        return 3;
    } finally {
        printf("fin\n");
    }
}

int main() {
    printf("f=%d\n", f());
    return 0;
}
