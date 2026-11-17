// const_global_init.mu — unit-level constants feed other constants and global initializers
unit const_global_init;

extern int puts(const char* s);

const int K = 7;
const int Twice = K * 2;
const int ConstFromMember = K;

int FromMember = K;                   // non-const global referring to a unit-level const
int FromArith = 1 + 2;                // constant expression in a non-const global
long FromWide = 5000000000;
double FromFloat = 1.5;
int FromConstGlobal = ConstFromMember + 1;
int FromTwice = Twice;

int main() {
    if FromMember != 7 { puts("global from const failed"); return 1; }
    if FromArith != 3 { puts("global arith failed"); return 1; }
    if FromWide != 5000000000 { puts("global wide failed"); return 1; }
    if FromFloat != 1.5 { puts("global float failed"); return 1; }
    if FromConstGlobal != 8 { puts("global from const global failed"); return 1; }
    if FromTwice != 14 { puts("global from const expr failed"); return 1; }
    puts("global init ok");

    // non-const global stays mutable
    FromMember = 42;
    if FromMember != 42 { puts("global not mutable"); return 1; }
    puts("global mutable ok");
    return 0;
}
