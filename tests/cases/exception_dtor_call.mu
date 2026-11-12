unit exception_dtor_call;

extern int printf(const char* fmt, ...);

int live = 0;

class Err : Exception {
    Err() { super("e"); }
}

class Guard {
    Guard() { live += 1; }
    void bump() { live -= 1; }
    ~Guard() { bump(); }
}

int boom() {
    Guard g = new Guard();
    throw new Err();
}

int main() {
    try {
        boom();
    } catch (e: Err) {
        printf("%d\n", live);
    }
    return 0;
}
