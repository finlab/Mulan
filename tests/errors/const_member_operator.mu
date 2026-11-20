unit const_member_operator;
struct Buf {
    int n;
    int[8] data;
    ref Buf operator<<(byte c) { data[n] = c; n = n + 1; return this; }
}
int main() { const Buf b = {}; b << 1; return b.n; }
