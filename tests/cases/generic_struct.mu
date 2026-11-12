// generic_struct.mu — generic struct monomorphization: Box<T>, Pair<K,V>, nested Box<Box<int>>
unit generic_struct;

extern int puts(const char* s);

struct Box<T> {
    T v;
}

struct Pair<A, B> {
    A first;
    B second;
}

int main() {
    Box<int> b;
    b.v = 42;

    Pair<int, long> p;
    p.first = 7;
    p.second = 9;

    Box<Box<int>> nested;
    nested.v.v = 3;

    int sum = b.v + p.first + (p.second as int) + nested.v.v;
    if (sum == 61) { puts("generic struct ok"); }
    return 0;
}
