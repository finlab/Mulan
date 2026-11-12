unit finally_loop;

extern int printf(const char* fmt, ...);

int main() {
    int i = 0;
    while i < 4 {
        try {
            i += 1;
            if i == 1 { continue; }
            if i == 2 { break; }
        } finally {
            printf("fin %d\n", i);
        }
    }
    printf("i=%d\n", i);
    return 0;
}
