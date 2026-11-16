unit is_iface;
use std.cstdio;

// `is` on an interface value: the value is a `{obj*, desc*}` aggregate, so the runtime test takes
// the object pointer first. The lowerer used to GEP the aggregate itself, producing invalid IR.
interface Shape { double area(); }
class Circle : Shape {
    double r;
    Circle(double rr) { r = rr; }
    override double area() { return 3.14 * r * r; }
}
class Square : Shape {
    double s;
    Square(double ss) { s = ss; }
    override double area() { return s * s; }
}

int main() {
    Shape c = new Circle(1.0);
    int hits = 0;
    if c is Circle {
        hits += 1;
    }
    if c is Square {
        printf("wrong match\n");
        return 1;
    }
    if c is Circle cc {
        if cc.area() > 3.0 {
            hits += 1;
        }
    }
    Shape n = null;
    if n is Circle {
        printf("null matched\n");
        return 2;
    }
    printf("hits=%d\n", hits);
    return 0;
}
