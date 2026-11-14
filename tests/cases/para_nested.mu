// para_nested — `para` inside a section; the enclosing local `x` must be forwarded through both
// frames (outer section frame -> inner section frame).

unit para_nested;

use std.cstdio;

class Box {
    public int a;
    public int b;
    public int c;
}

int f(int x) { return x * 2; }

int main() {
    Box o = new Box();
    int x = 5;
    para {
        { para { { o.a = f(x); } { o.b = f(x + 1); } } }
        { o.c = f(x + 2); }
    }
    printf("%d %d %d\n", o.a, o.b, o.c);
    return 0;
}
