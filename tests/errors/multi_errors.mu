// multi_errors.mu — one compilation reports several errors (instead of stopping at the first)
unit multi_errors;

int f1() { return undefined_one; }
int f2() { return undefined_two; }

struct S {
    int a;
}

int f3(S s) { return s.nope; }

int main() { return 0; }
