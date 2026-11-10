unit union_rc_struct_field;
class C { int x; }
struct Holder { C c; }
union U { int i; Holder h; }
int main() { return 0; }
