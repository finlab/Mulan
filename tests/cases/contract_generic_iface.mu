// contract_generic_iface.mu — a generic contract keeps mixed operands and a `ref` receiver
unit contract_generic_iface;

extern int puts(const char* s);

interface IOut<T> { Self operator<<(ref Self b, T c); }

struct Buf { int n; }

Buf operator<<(ref Buf b, int c) { b.n = b.n * 10 + c; return b; }

void feed<T: IOut<int>>(ref T b) {
    b << 1;
    b << 2;
}

int main() {
    Buf b = {};
    feed<Buf>(b);
    if b.n != 12 { return 1; }
    puts("generic interface operator ok");
    return 0;
}
