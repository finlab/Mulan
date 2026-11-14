public unit exportlib.api;

extern int puts(const char* s);

export int twice(int x) { return x * 2; }

export class Shape {
public:
    int side;
    Shape(int n) { side = n; }
    int area() { return side * side; }
}
