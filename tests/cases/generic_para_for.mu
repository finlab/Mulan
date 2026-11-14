// `para for` inside a generic function (same clone table as `para`, issue #7).
unit generic_para_for;
extern int printf(const char* fmt, ...);

class Box {
    public int a;
    public int b;
    public int c;
}

void spread<T>(Box box, T v) {
    para for i in 0..3 {
        if i == 0 { box.a = 1; }
        if i == 1 { box.b = 1; }
        if i == 2 { box.c = 1; }
    }
    if v == v {
    }
}

int main() {
    Box x = new Box();
    Box y = new Box();
    spread<int>(x, 7);
    spread<long>(y, 9);
    printf("%d %d %d %d %d %d\n", x.a, x.b, x.c, y.a, y.b, y.c);
    return 0;
}
