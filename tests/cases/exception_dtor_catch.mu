unit exception_dtor_catch;

extern int printf(const char* fmt, ...);

int hits = 0;

class E : Exception {
    E() { super("e"); }
}

void risky() {
    throw new E();
}

class G {
    ~G() {
        try {
            risky();
        } catch (e: E) {
            hits += 1;
        }
    }
}

int boom() {
    G g = new G();
    throw new E();
}

int main() {
    try {
        boom();
    } catch (e: E) {
        printf("%d\n", hits);
    }
    return 0;
}
