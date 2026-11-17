// if_body_cond.mu — a statement/type-body `#if` is evaluated by the semantic pass, so it reads the
// constants of a `use`d unit, nested chains work, the field order around a group is kept (layout),
// and a dead arm's `#error` never fires.
unit if_body_cond;

use iflib.lib;

extern int puts(const char* s);

struct S {
    byte a;
#if K == 1 {
    long b;
}
#elif K == 7 {
    int b;
}
#else {
#error "the struct arm was not selected"
}
    byte c;
}

T twice<T>(T x) {
#if K == 7 {
    return x + x;
}
#else {
    return x;
}
}

int main() {
    S s;
    s.a = 1;
    s.b = 2;
    s.c = 3;
    // Field order is layout: a@0, b@4, c@8 -> 12 bytes. An arm appended at the end of the body
    // would give a@0, c@1, b@4 -> 8 bytes.
    if sizeof(S) != 12 { puts("member order lost"); return 1; }

#if iflib.lib.K == 7 {
#if K == 7 {
    const int N = 3;
}
#else {
    const int N = 4;
}
}
#else {
#error "the cross-unit arm was not selected"
}

    if N != 3 { puts("nested body condition failed"); return 2; }
    if twice<int>(21) != 42 { puts("generic if body failed"); return 3; }
    puts("body cond ok");
    return 0;
}
