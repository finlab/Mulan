unit varargs_slice_arg;
extern int printf(const char* fmt, ...);
int main() {
    int[] xs = new int[3];
    printf("%d\n", xs);
    return 0;
}
