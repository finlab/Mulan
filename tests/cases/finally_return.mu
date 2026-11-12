unit finally_return;

extern int printf(const char* fmt, ...);

int f() {
    try {
        return 1;
    } finally {
        printf("fin\n");
    }
}

int main() {
    printf("f=%d\n", f());
    return 0;
}
