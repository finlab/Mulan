// contract_stream_ops.mu — a contract operator may take operands that are not `Self`
// (`Self << byte`, `Self * int`, `Self < int`); only the receiver has to be `Self`.
unit contract_stream_ops;

extern int puts(const char* s);

interface IOut { Self operator<<(Self b, byte c); }
interface IScale { Self operator*(Self a, int k); }
interface IOrderInt { bool operator<(Self a, int b); }

// class: the free operator mutates through the shared handle and returns it (chaining works)
class Buf {
public:
    int n;
    Buf() { n = 0; }
}

Buf operator<<(Buf b, byte c) { b.n = b.n * 10 + c; return b; }

struct Vec2 {
    int x;
    Vec2 operator*(int k) { return Vec2(x * k); }
}

struct Cmp {
    int n;
    bool operator<(int b) { return n < b; }
}

T emit<T: IOut>(T b) { return b << 1 << 2; }
T scale<T: IScale>(T v, int k) { return v * k; }
bool below<T: IOrderInt>(T a, int b) { return a < b; }

int main() {
    Buf b = new Buf();
    Buf c = emit<Buf>(b);
    if c.n != 12 { return 1; }

    Vec2 v = {3};
    if scale<Vec2>(v, 2).x != 6 { return 2; }

    Cmp cmp = {1};
    if not below<Cmp>(cmp, 2) { return 3; }

    puts("contract mixed operators ok");
    return 0;
}
