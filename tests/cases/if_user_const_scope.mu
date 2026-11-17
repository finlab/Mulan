// if_user_const_scope.mu — a unit-level constant is visible to `#if` inside a struct body and a
// function body once it has been declared earlier in the unit.
unit if_user_const_scope;

extern int puts(const char* s);

const int N = 1;
const int M = 2 as int;

struct S {
    #if N == 1 {
        int a;
    }
    #else {
        int b;
    }
}

int main() {
    S s = {7};
    #if N == 1 {
        if s.a != 7 { puts("struct member condition failed"); return 1; }
    }
    #if M == 2 {
        puts("if scope ok");
    }
    #else {
        return 2;
    }
    return 0;
}
