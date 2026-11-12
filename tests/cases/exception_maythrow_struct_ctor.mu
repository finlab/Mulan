unit exception_maythrow_struct_ctor;

extern int printf(const char* fmt, ...);

class E : Exception {
    E(string m) { super(m); }
}

struct S {
    int x;
    S(int v) {
        x = v;
        throw new E("s");
    }
}

int main() {
    try {
        S s = S(1);
        printf("not reached\n");
    } catch (e: E) {
        printf("caught %d\n", e.message.length() as int);
    }
    return 0;
}
