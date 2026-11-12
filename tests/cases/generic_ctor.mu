// generic_ctor.mu — 泛型 struct 的构造器调用（`Pair<int,long>(...)`）
unit generic_ctor;

extern int puts(const char* s);

struct Pair<A, B> {
    A first;
    B second;

    Pair(A a, B b) {
        first = a;
        second = b;
    }
}

struct Vec2<T> {
    T x;
    T y;

    Vec2(T a, T b) {
        x = a;
        y = b;
    }

    T dot() { return x * x + y * y; }
}

int main() {
    Pair<int, long> p = Pair<int, long>(3, 9);
    Vec2<int> v = Vec2<int>(3, 4);
    int d = v.dot();

    if (p.first == 3 and p.second == 9 and d == 25) { puts("generic ctor ok"); }
    return 0;
}
