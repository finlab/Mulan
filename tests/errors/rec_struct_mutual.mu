// Two structs storing each other by value have no finite layout. The cycle used to pass the semantic
// check and only surface in codegen as "field is not a fixed-size type"; `check_layouts` now reports
// it, with the cycle spelled out.
unit rec_struct_mutual;

struct A { B b; }
struct B { A a; }

int main() { return 0; }
