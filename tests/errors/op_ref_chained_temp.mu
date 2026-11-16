unit op_ref_chained_temp;

struct Buf { int n; }

// value result: chaining would hand the first result (a temporary) to a `ref` receiver
Buf operator<<(ref Buf b, byte c) { b.n = b.n * 10 + c; return b; }

void feed(ref Buf b) { b << 1 << 2; }

int main() {
    Buf b = {};
    feed(b);
    return 0;
}
