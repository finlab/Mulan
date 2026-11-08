unit lib_shape;
use lib_iface;

class Rect : lib_iface.IShape {
    int w, h;
    Rect(int a, int b) { w = a; h = b; }
    ~Rect() { }
    override int area() { return w * h; }
}

class Square : Rect {
    Square(int s) { super(s, s); }
    ~Square() { }
}
