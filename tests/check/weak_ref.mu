unit check_weak_ref;

// weak fields, interface default methods, abstract classes, inheritance and super

interface IShape {
    int area();
    bool is_empty() { return area() == 0; }
}

class Node {
    int id;
    weak Node next;      // weak reference, does not increment the reference count
    Node(int i) { id = i; next = null; }
    ~Node() { }

    void link(Node other) { next = other; }
    Node peek() { return next; }
}

class Shape : IShape {
    int sid;
    Shape() { sid = 0; }
    Shape(int i) { sid = i; }
    ~Shape() { }
    override int area() { return 0; }
    override bool is_empty() { return true; }
}

class Circle : Shape {
    int r;
    Circle(int r0) { super(r0); r = r0; }
    override int area() { return 3 * r * r; }
}

int use_shape(IShape s) { return s.area(); }

int main() {
    Node a = new Node(1);
    Node b = new Node(2);
    a.link(b);
    Node c = a.peek();
    c = null;
    b = null;
    a = null;

    Circle circ = new Circle(2);
    int x = use_shape(circ);
    bool e = circ.is_empty();
    circ = null;
    return x;
}
