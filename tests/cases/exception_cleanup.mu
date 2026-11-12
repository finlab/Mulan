unit exception_cleanup;

extern int printf(const char* fmt, ...);

int live = 0;

class Guard {
    int tag;
    Guard(int t) { tag = t; live += 1; }
    ~Guard() { live -= 1; }
}

class Err : Exception {
    Err() { super("e"); }
}

int boom() {
    Guard a = new Guard(1);
    Guard b = new Guard(2);
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
