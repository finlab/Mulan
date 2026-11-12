unit finally_rc;

extern int printf(const char* fmt, ...);

int live = 0;

class Guard {
    Guard() { live += 1; }
    ~Guard() { live -= 1; }
}

class E : Exception {
    E() { super("e"); }
}

int main() {
    try {
        Guard g = new Guard();
        throw new E();
    } catch (e: E) {
        printf("in %d\n", live);
    } finally {
        printf("fin %d\n", live);
    }
    printf("out %d\n", live);
    return 0;
}
