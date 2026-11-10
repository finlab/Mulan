unit lib_shape;
use lib_iface;

public class Rect : lib_iface.IShape {
    int w, h;
    public Rect(int a, int b) { w = a; h = b; }
    ~Rect() { }
    override int area() { return w * h; }
}

public class Square : Rect {
    public Square(int s) { super(s, s); }
    ~Square() { }
}
