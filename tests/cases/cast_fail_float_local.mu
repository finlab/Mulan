// cast_fail_float_local — an out-of-range `double as int` in a function that also has a `float`
// local keeps its `cast__fail` call valid: the hook takes the source value (double), never the
// neighbouring float slot. The path is emitted but not taken, because taking it aborts the process.
unit cast_fail_float_local;

use std.cstdio;

int maybe_cast(double d, int flag) {
    if flag != 0 { return d as int; }
    return 0;
}

int main() {
    float f = 2.5;
    double d = 1e30;
    int x = maybe_cast(d, 0);
    if x != 0 { printf("cast taken\n"); return 1; }
    printf("%.1f\n", f);
    return 0;
}
