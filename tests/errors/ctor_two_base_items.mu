// The construction prologue names the base constructor at most once (SPEC 3.12).
unit ctor_two_base_items;
class A { public int v; A(int x) { v = x; } A(int x, int y) { v = x + y; } }
class B : A { B() : A(1), A(2, 3) { } }
int main() { return 0; }
