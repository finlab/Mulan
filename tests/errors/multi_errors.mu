// multi_errors.mu — 一次编译报出多个错误（而不是遇到第一个就停）
unit multi_errors;

int f1() { return undefined_one; }
int f2() { return undefined_two; }

struct S {
    int a;
}

int f3(S s) { return s.nope; }

int main() { return 0; }
