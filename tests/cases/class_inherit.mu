unit class_inherit;

extern int puts(const char* s);

interface IShape {
    int area();
    bool is_empty() { return area() == 0; }
}

class Shape : IShape {
    int id;
    Shape(int i) { id = i; }
    ~Shape() { }
    override int area() { return 0; }
}

class Rect : Shape {
    int w;
    int h;
    Rect(int a, int b) { super(1); w = a; h = b; }
    ~Rect() { }
    override int area() { return w * h; }
}

class Square : Rect {
    Square(int s) { super(s, s); }
    ~Square() { }
}

int area_of(Shape s) { return s.area(); }
int area_iface(IShape s) { return s.area(); }

int main() {
    Square q = new Square(4);
    if q.area() != 16 { puts("direct dispatch failed"); return 1; }
    if q.id != 1 { puts("super ctor failed"); return 1; }

    Shape s = q;                       // implicit upcast + strong reference
    if s.area() != 16 { puts("virtual dispatch failed"); return 1; }

    IShape i = s;                      // class -> interface
    if i.area() != 16 { puts("interface dispatch failed"); return 1; }
    if i.is_empty() { puts("default method failed"); return 1; }

    if area_of(s) != 16 { puts("base param failed"); return 1; }
    if area_iface(i) != 16 { puts("iface param failed"); return 1; }

    q = null;
    if i.area() != 16 { puts("rc keepalive failed"); return 1; }
    s = null;
    if i.area() != 16 { puts("iface keepalive failed"); return 1; }
    i = null;

    puts("class inherit ok");
    return 0;
}
