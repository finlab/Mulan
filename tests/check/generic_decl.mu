unit generic_decl;

interface IComparable {
    bool less(Self other);
}

interface ISeq<T> {
    usize length();
    T at(usize i);
}

struct Vec<T> : ISeq<T> {
    T[] data;
}

class Box<T> : IComparable {
    T v;
    Box(T x) { v = x; }
    override bool less(Box<T> other) { return v < other.v; }
}

struct Nested<T> {
    Vec<Vec<T>> deep;
}

T max<T: IComparable>(T a, T b) { return a.less(b) ? b : a; }

T sum<T: int | long>(T[] xs) {
    T s = 0;
    for x in xs { s += x; }
    return s;
}

T both<T: IComparable & ISeq<int>>(T x) { return x; }

int main() { return 0; }
