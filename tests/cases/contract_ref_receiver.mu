// contract_ref_receiver.mu — a contract operator receiver declared `ref Self` (mutate in place),
// implemented by a free function whose first parameter borrows the receiver.
unit contract_ref_receiver;

extern int puts(const char* s);

interface IOut { Self operator<<(ref Self b, byte c); }

struct Buf { int n; }

Buf operator<<(ref Buf b, byte c) { b.n = b.n * 10 + c; return b; }

void feed<T: IOut>(ref T b) {
    b << 1;
    b << 2;
}

int main() {
    Buf b = {};
    feed<Buf>(b);
    if b.n != 12 { return 1; }
    puts("ref receiver contract ok");
    return 0;
}
