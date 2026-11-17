// if_const_arm_value.mu — the value a `#if` reads belongs to the declaration that was selected (not to
// a same-named constant of a dead arm), and a `sizeof` constant is visible to a function-body `#if`.
unit if_const_arm_value;

extern int puts(const char* s);

struct P { int x; int y; }

const int W = sizeof(P) as int;

#if W == 8 {
    const int B = 2;
}
#else {
    const int B = 3;
}

int main() {
#if W == 8 {
    const int C = 1;
}
#else {
    const int C = 0;
}
#if B == 2 {
    const int D = 1;
}
#else {
    const int D = 0;
}
    if D != 1 { puts("dead-arm value leaked"); return 1; }
    if C != 1 { puts("sizeof constant not visible in a body"); return 2; }
    puts("const arm value ok");
    return 0;
}
