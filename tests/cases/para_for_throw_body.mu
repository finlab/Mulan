// A throw that escapes a `para for` item body aborts the process (SPEC 3.17.4, B2).
unit para_for_throw_body;
extern int printf(const char* fmt, ...);

class Err : Exception {
    Err() { super("item"); }
}

int main() {
    para for i in 0..2 {
        if i == 1 {
            printf("item throws\n");
            throw new Err();
        }
    }
    printf("unreachable\n");
    return 0;
}
