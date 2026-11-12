unit finally_nomatch;

extern int printf(const char* fmt, ...);

class E : Exception {
    E(string m) { super(m); }
}

int main() {
    try {
        try {
            throw new E("x");
        } finally {
            printf("fin\n");
        }
        printf("not reached\n");
    } catch (e: E) {
        printf("outer\n");
    }
    return 0;
}
