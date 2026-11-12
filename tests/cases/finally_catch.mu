unit finally_catch;

extern int printf(const char* fmt, ...);

class E : Exception {
    E(string m) { super(m); }
}

int main() {
    try {
        throw new E("x");
    } catch (e: E) {
        printf("catch\n");
    } finally {
        printf("fin\n");
    }
    printf("after\n");
    return 0;
}
