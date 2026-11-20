unit generic_struct_class_base;
class C { public int x; }
struct Bad<T> : C { T v; Bad() {} }
int main() { Bad<int> b = {}; return 0; }
