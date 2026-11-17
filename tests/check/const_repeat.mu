// const_repeat.mu — a repeated `const` folds (SPEC 3.7): a declaration may repeat the qualifier, and
// repeated qualifiers at different levels are the same as one.
unit const_repeat;

const const int A = 5;
const const int* P = null;
int* const const Q = null;

struct S {
    const const int v;
}

S make(int v) { S s; s = S(); return s; }

int main() {
    if A != 5 { return 1; }
    if P != null { return 2; }
    if Q != null { return 3; }
    return 0;
}
