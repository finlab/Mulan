// const_global_init.mu — non-const unit-level vars can also be initialized with compile-time constants
//
// Covers: referring to type-level const members, constant arithmetic exprs, each scalar type, and non-const globals staying mutable.
unit const_global_init;

extern int puts(const char* s);

struct Cfg {
    const int K = 7;
    const int Twice = K * 2;
}

const int ConstFromMember = Cfg.K;

int FromMember = Cfg.K;               // non-const global referring to a const member
int FromArith = 1 + 2;                // constant expression in a non-const global
long FromWide = 5000000000;
double FromFloat = 1.5;
int FromConstGlobal = ConstFromMember + 1;
int FromTwice = Cfg.Twice;

int main() {
    if FromMember != 7 { puts("global from member failed"); return 1; }
    if FromArith != 3 { puts("global arith failed"); return 1; }
    if FromWide != 5000000000 { puts("global wide failed"); return 1; }
    if FromFloat != 1.5 { puts("global float failed"); return 1; }
    if FromConstGlobal != 8 { puts("global from const global failed"); return 1; }
    if FromTwice != 14 { puts("global from member expr failed"); return 1; }
    puts("global init ok");

    // non-const global stays mutable
    FromMember = 42;
    if FromMember != 42 { puts("global not mutable"); return 1; }
    puts("global mutable ok");
    return 0;
}
