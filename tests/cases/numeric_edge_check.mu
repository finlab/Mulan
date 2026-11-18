// A shift count outside [0, width) and a float->int `as` that does not fit are UB at run time; the
// default build (`--bounds=on`) reports both as `std.exception.NumericEdgeError` instead of letting the
// optimizer produce a different answer per run (decided 2026-11-18).
unit numeric_edge_check;

extern int printf(const char* fmt, ...);

int main() {
    int n = -1;
    double big = 1.0e30;
    try {
        int b = 1 << n;
        printf("no throw %d\n", b);
        return 1;
    } catch (e: NumericEdgeError) {
        printf("shift kind=%d width=%d\n", e.kind, e.width as int);
    }
    try {
        int a = big as int;
        printf("no throw %d\n", a);
        return 1;
    } catch (e: NumericEdgeError) {
        printf("cast kind=%d width=%d\n", e.kind, e.width as int);
    }
    return 0;
}
