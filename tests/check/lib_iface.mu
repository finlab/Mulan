unit lib_iface;

public interface IShape {
    int area();
    bool empty() { return area() == 0; }
}
