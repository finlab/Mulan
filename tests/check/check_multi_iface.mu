unit check_multi_iface;
use lib_iface;
use lib_shape;

int use_shape(lib_iface.IShape s) { return s.area(); }

int main() {
    lib_shape.Square q = new lib_shape.Square(5);
    lib_iface.IShape s = q;
    int a = s.area();
    int b = use_shape(s);
    bool e = s.empty();
    del q;
    int c = s.area();
    del s;
    return a + b + c;
}
