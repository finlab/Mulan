// main_args.mu — main with args: argc / argv (C ABI, argv is null-terminated)
unit main_args;

extern int printf(const char* fmt, ...);

int main(int argc, char** argv) {
    if argv == null {
        printf("argv is null\n");
        return 1;
    }
    // argv is null-terminated: element count must match argc
    int n = 0;
    while argv[n] != null {
        n += 1;
    }
    if n != argc {
        printf("argv count %d != argc %d\n", n, argc);
        return 2;
    }
    if argc < 1 {
        printf("argc < 1\n");
        return 3;
    }
    printf("main args ok\n");
    return 0;
}
