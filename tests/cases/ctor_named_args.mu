unit ctor_named_args;

// Named arguments select the constructor overload by parameter name and are reordered to positional
// order, for both `T(...)` and `new T(...)`.

use std.cstdio;

struct Point {
    int x;
    int y;
    Point(int a, int b) { x = a; y = b; }
}

class Box {
public:
    int v;
    Box(int a) { v = a; }
}

int main() {
    Point p = Point(b = 2, a = 1);
    Box bx = new Box(a = 7);
    printf("%d %d %d\n", p.x, p.y, bx.v);
    return p.x == 1 and p.y == 2 and bx.v == 7 ? 0 : 1;
}
