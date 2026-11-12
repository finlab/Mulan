unit exception_loop;

extern int printf(const char* fmt, ...);

class Err : Exception {
    Err() { super("loop"); }
}

int main() {
    int i = 0;
    while i < 3 {
        try {
            if i == 1 { throw new Err(); }
            printf("ok%d\n", i);
        } catch (e: Err) {
            printf("caught%d\n", i);
        }
        i += 1;
    }
    return 0;
}
