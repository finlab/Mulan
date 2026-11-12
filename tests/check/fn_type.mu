// fn_type.mu — function type syntax in parameter / field / generic / sizeof positions (semantic check only)
unit fn_type;

type Cmp = (const void*, const void*) => int;
type Op = (int, int) => int;
type Thunk = () => void;
type Unary = (int) => int;
type Fact = (int) => Unary;   // right-associative: `(int) => (int) => int`

struct Handlers {
    Op op;
    Cmp cmp;
    Thunk tick;
}

int inc(int v) { return v + 1; }

Unary to_unary(int k) { return inc; }   // has type `(int) => Unary`, i.e. Fact

T choose<T>(T a, T b, (T, T) => bool less) {
    if (less(a, b)) { return a; }
    return b;
}

void run(Op f, (int) => bool pred, Fact maker) { }

// a lambda inside a generic body is instantiated per monomorphization
T ident<T>(T v) {
    auto id = (T x) => x;
    return id(v);
}

// the synthesized name of a lambda must not collide with a user function that sanitizes to the same symbol
int _lambda_1(int x) { return x + 100; }

int main() {
    Handlers h;
    h.op = (int a, int b) => a + b;
    h.cmp = null;
    h.tick = () => { };
    int n = sizeof(Op) as int;      // 8
    int m = alignof(Thunk) as int;  // 8
    int k = sizeof(Fact) as int;    // 8
    Op op = h.op;
    run(op, (int v) => v > 0, to_unary);
    auto r = choose<int>(1, 2, (int a, int b) => a < b);
    auto same = ident<int>(7);
    auto bumped = (int x) => x + 1;
    return n + m + k + r + same + bumped(1) + _lambda_1(0) - 100;
}
