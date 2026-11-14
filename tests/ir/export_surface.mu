unit export_surface;

extern int puts(const char* s);

public int pub_fn(int x) { return x + 1; }

export int exp_fn(int x) { return x + 2; }

export class Shape {
public:
    int side;
    Shape(int n) { side = n; }
    int area() { return side * side; }
}

int main() {
    puts("hi");
    return 0;
}
