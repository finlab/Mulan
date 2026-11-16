unit para_mono_return;
// A generic body is checked when it is instantiated. The first instantiation may happen inside a
// `para` section; the body's own `return` belongs to that generic, not to the section, so it must
// not be rejected with "return cannot leave a `para` section".
T ident<T>(T x) {
    return x;
}

T pick<T>(T a, T b) {
    if a == b {
        return a;
    }
    return b;
}

int main() {
    para {
        {
            int v = ident<int>(6);
            if v == 0 {
                v = 1;
            }
            int w = pick<int>(2, 3);
            if w == 0 {
                w = 2;
            }
        }
    }
    return 0;
}
