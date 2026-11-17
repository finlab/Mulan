// if_sizeof_const.mu — a `const` initialized with `sizeof` is a compile-time constant and is visible
// to `#if` (the parser folds literals; collect evaluates the `sizeof` form).
unit if_sizeof_const;

extern int puts(const char* s);

const int W = sizeof(int) as int;
const int L = sizeof(long) as int;

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
    puts("sizeof const ok");
    return 0;
}
