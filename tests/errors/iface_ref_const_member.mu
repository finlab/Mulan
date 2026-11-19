unit iface_ref_const_member;

interface IRead { int operator<<(const ref Self b, byte c); }

struct Buf {
    int n;
    // a member operator has a writable `this`; it cannot promise a read-only receiver
    int operator<<(byte c) { n = 0; return n; }
}

int go<T: IRead>(ref T b) { return b << 1; }

int main() {
    Buf b = {};
    return go<Buf>(b);
}
