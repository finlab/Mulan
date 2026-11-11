// const_global_init.mu — 非 const unit 级变量也可以用编译期常量初始化
//
// 覆盖：引用类型级常量成员、常量算术表达式、各标量类型，以及非 const 全局可变。
unit const_global_init;

extern int puts(const char* s);

struct Cfg {
    const int K = 7;
    const int Twice = K * 2;
}

const int ConstFromMember = Cfg.K;

int FromMember = Cfg.K;               // 非 const 全局引用常量成员
int FromArith = 1 + 2;                // 非 const 全局的常量表达式
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

    // 非 const 全局仍然可变
    FromMember = 42;
    if FromMember != 42 { puts("global not mutable"); return 1; }
    puts("global mutable ok");
    return 0;
}
