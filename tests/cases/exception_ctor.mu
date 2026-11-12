unit exception_ctor;

extern int printf(const char* fmt, ...);

int live = 0;

class Boom : Exception {
    Boom(string m) { super(m); }
}

class C {
    int x;
    C(int y) {
        live += 1;
        x = y;
        throw new Boom("ctor");
    }
    ~C() { live -= 1; }
}

int main() {
    try {
        C c = new C(7);
        printf("not reached %d\n", c.x);
    } catch (e: Boom) {
        printf("caught %d\n", live);
    }
    return 0;
}
