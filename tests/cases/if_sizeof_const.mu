// if_sizeof_const.mu — a `const` initialized with `sizeof` is a compile-time constant and is visible
// to `#if` (the parser folds literals; collect evaluates the `sizeof` form).
unit if_sizeof_const;

extern int puts(const char* s);

const int W = sizeof(int) as int;
const int L = sizeof(long) as int;
struct P { int x; int y; }
const int S = sizeof(P) as int;

#if S == 8 {
    const int C = 1;
}
#else {
    const int C = 2;
}

#if W == 4 {
    const int A = 1;
}
#else {
    const int A = 2;
}

#if L == 8 {
    const int B = 1;
}
#else {
    const int B = 2;
}

int main() {
    if A != 1 or B != 1 { puts("sizeof constant condition failed"); return 1; }
    if C != 1 { puts("struct sizeof condition failed"); return 2; }
    puts("sizeof const ok");
    return 0;
}
