unit exception_owner;

extern int printf(const char* fmt, ...);

int live = 0;

class E : Exception {
    E() { super("e"); live += 1; }
    ~E() { live -= 1; }
}

int main() {
    try {
        throw new E();
    } catch (e: E) {
        printf("in %d\n", live);
    }
    printf("out %d\n", live);
    return 0;
}
