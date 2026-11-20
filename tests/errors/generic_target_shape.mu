unit generic_target_shape;
struct Box<T> { T v; }
Box<T> make_box<T>() { Box<T> b = {}; return b; }
int main() { int x = make_box(); return x; }
