unit ctor_named_args_unknown;

struct Point {
    int x;
    Point(int a) { x = a; }
}

int main() {
    Point p = Point(x = 1);
    return p.x;
}
