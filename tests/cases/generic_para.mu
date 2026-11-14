// A `para` block inside a generic function survives monomorphization (issue #7: the AST clone
// used to drop the whole statement, so the generic version silently did nothing). Instantiating
// with two different type arguments covers both cloned bodies.
unit generic_para;
extern int printf(const char* fmt, ...);

class Box {
    public int a;
    public int b;
}

void fill<T>(Box box, T v) {
    para {
        { box.a = 1; }
        { box.b = 2; }
    }
    if v == v {
        // keep `v` alive in the body so the generic parameter is really used
    }
}

int main() {
    Box x = new Box();
    Box y = new Box();
    fill<int>(x, 3);
    fill<double>(y, 0.5);
    printf("%d %d %d %d\n", x.a, x.b, y.a, y.b);
    return 0;
}
