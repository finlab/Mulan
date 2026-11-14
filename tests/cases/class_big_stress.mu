unit class_big_stress;
extern int puts(const char* s);
int live = 0;
interface IShape { int area(); int id(); }
class Shape : IShape {
    int sid;
    Shape(int i) { sid = i; live += 1; }
    ~Shape() { live -= 1; }
    override int area() { return 0; }
    override int id() { return sid; }
}
class Rect : Shape {
    int w, h;
    Rect(int a, int b) { super(2); w = a; h = b; }
    ~Rect() { }
    override int area() { return w * h; }
}
class Square : Rect {
    Square(int s) { super(s, s); }
    ~Square() { }
}
class Box {
    IShape[2] items;
    Box() { items[0] = null; items[1] = null; }
    ~Box() { }
    void set(int i, IShape s) { items[i] = s; }
    IShape get(int i) { return items[i]; }
    int total() { return items[0].area() + items[1].area(); }
}
int main() {
    Box b = new Box();
    b.set(0, new Square(3));
    b.set(1, new Rect(2, 3));
    if b.total() != 15 { puts("total fail"); return 1; }
    if live != 2 { puts("live after set fail"); return 1; }

    int sum = 0;
    for i in 0..2 { sum += b.items[i].area(); }
    if sum != 15 { puts("for-in iface array fail"); return 1; }
    if live != 2 { puts("for-in leak"); return 1; }

    Shape s = b.get(0) as Shape;
    if s == null { puts("downcast shape fail"); return 1; }
    Square q = b.get(0) as Square;
    if q == null { puts("downcast square fail"); return 1; }
    if q.id() != 2 { puts("downcast id fail"); return 1; }
    Rect r = b.get(1) as Rect;
    if r == null or r.area() != 6 { puts("downcast rect fail"); return 1; }
    s = null;
    q = null;
    r = null;
    if live != 2 { puts("downcast rc fail"); return 1; }

    b.set(0, null);
    if b.get(0) != null { puts("set null fail"); return 1; }
    if b.get(1).area() != 6 { puts("set null other fail"); return 1; }
    b = null;
    if live != 0 { puts("box cleanup fail"); return 1; }
    puts("big stress ok");
    return 0;
}


