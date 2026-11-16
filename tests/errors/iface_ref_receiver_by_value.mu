unit iface_ref_receiver_by_value;

interface IOut { Self operator<<(ref Self b, byte c); }

struct Buf { int n; }

// by-value receiver: it would only mutate a copy, so it cannot satisfy `ref Self`
Buf operator<<(Buf b, byte c) { b.n = b.n * 10 + c; return b; }

void feed<T: IOut>(ref T b) { b << 1; }

int main() {
    Buf b = {};
    feed<Buf>(b);
    return 0;
}
