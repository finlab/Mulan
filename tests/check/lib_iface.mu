unit lib_iface;

interface IShape {
    int area();
    bool empty() { return area() == 0; }
}
