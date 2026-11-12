// generic_methods.mu — 泛型 struct 的方法实例化（U34 M3d）
unit generic_methods;

extern int puts(const char* s);

struct Vec<T> {
    T[] data;

    usize length() { return len(data); }

    T at(usize i) { return data[i]; }

    void fill(T v) {
        for ref x in data { x = v; }
    }
}

struct Wrap<T> {
    T v;

    T twice() { return v + v; }
}

int main() {
    Vec<int> v = {new int[3]};
    v.fill(7);
    int s = v.at(0) + v.at(1) + v.at(2);
    usize n = v.length();

    Wrap<int> w = {21};
    int d = w.twice();

    if (s == 21 and n == 3 and d == 42) { puts("generic methods ok"); }
    return 0;
}
