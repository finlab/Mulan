// extend_basic — `extend I with J;` (SPEC 3.18): implementors of `I` gain `J`'s default methods,
// for a struct and a class, through a library unit that declares the extension.
unit extend_basic;

use extlib.mixin;
use std.cstdio;

struct Square : IShape {
    public int side;
    override int area() { return side * side; }
}

class Circle : IShape {
    public int r;
    override int area() { return 3 * r * r; }
}

int main() {
    Square s;
    s.side = 3;
    printf("%d %d\n", s.doubled(), s.scaled(10));

    Circle c = new Circle();
    c.r = 2;
    printf("%d %d\n", c.doubled(), c.scaled(5));
    return 0;
}
