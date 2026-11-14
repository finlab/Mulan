// sizeof_expr — `sizeof`/`alignof` take a value as well as a type (C's paren-free form and
// `sizeof(expr)`), and `offsetof(T, field)` folds to the model layout.

unit sizeof_expr;

use std.cstdio;

struct S {
    char a;
    int b;
    long c;
}

int main() {
    int x = 0;
    int* p = &x;
    int[4] arr = {0, 0, 0, 0};
    S s = {};

    printf("%d %d %d %d\n", sizeof(int) as int, sizeof(x) as int, sizeof *p as int,
           sizeof(arr[0]) as int);
    printf("%d %d %d\n", sizeof(arr) as int, sizeof(s) as int, sizeof(S) as int);
    printf("%d %d %d\n", offsetof(S, a) as int, offsetof(S, b) as int, offsetof(S, c) as int);
    printf("%d %d\n", alignof(S) as int, alignof(char) as int);
    return 0;
}
