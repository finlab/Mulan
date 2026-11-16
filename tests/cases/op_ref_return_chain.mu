// op_ref_return_chain.mu — an operator may return `ref T`; chaining then borrows the receiver
unit op_ref_return_chain;

extern int puts(const char* s);

struct Buf { int n; }

ref Buf operator<<(ref Buf b, int c) {
    b.n = b.n * 10 + c;
    return b;
}

int main() {
    Buf b = {};
    b << 1 << 2 << 3;
    if b.n != 123 { return 1; }
    puts("operator ref return chain ok");
    return 0;
}
