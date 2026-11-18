// `--bounds=off` drops the numeric-edge checks entirely (the operations are then UB outside the range,
// documented in SPEC 8.2). In-range code must be unaffected.
unit numeric_edge_unchecked;

extern int printf(const char* fmt, ...);

int main() {
    double d = 41.9;
    int n = 3;
    printf("%d %d\n", d as int, 1 << n);
    return 0;
}
