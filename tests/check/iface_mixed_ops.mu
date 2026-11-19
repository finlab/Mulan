// iface_mixed_ops.mu — contract operator members: mixed operands, `ref` and `const ref` receivers
unit iface_mixed_ops;

interface IOut { Self operator<<(Self b, byte c); }
interface IScale { Self operator*(Self a, int k); }
interface IOrderInt { bool operator<(Self a, int b); }
interface IRefOut { Self operator<<(ref Self b, byte c); }
interface IReadOut { int operator<<(const ref Self b, byte c); }

// by-value receiver: a member operator (the receiver is the implicit `this`)
struct ValBuf {
    int n;
    ValBuf operator<<(byte c) { return ValBuf(n * 10 + c); }
}

struct ValVec {
    int x;
    ValVec operator*(int k) { return ValVec(x * k); }
}

struct Cmp {
    int n;
    bool operator<(int b) { return n < b; }
}

// `ref Self` receiver: a free operator borrows the receiver and mutates it
struct RefBuf { int n; }
RefBuf operator<<(ref RefBuf b, byte c) { b.n = b.n * 10 + c; return b; }

// `const ref Self` receiver: read-only, so a `const ref` free operator qualifies
struct ReadBuf { int n; }
int operator<<(const ref ReadBuf b, byte c) { return b.n + c; }

T emit<T: IOut>(T b) { return b << 1; }
T scale<T: IScale>(T v, int k) { return v * k; }
bool below<T: IOrderInt>(T a, int b) { return a < b; }
void feed<T: IRefOut>(ref T b) { b << 1; }
int read<T: IReadOut>(ref T b) { return b << 1; }

int main() {
    ValBuf b = {};
    ValBuf c = emit<ValBuf>(b);
    ValVec v = {2};
    ValVec w = scale<ValVec>(v, 3);
    Cmp cmp = {1};
    RefBuf rb = {};
    feed<RefBuf>(rb);
    ReadBuf rd = {};
    int r = read<ReadBuf>(rd);
    return c.n + w.x + r + (below<Cmp>(cmp, 2) ? 0 : 1);
}
