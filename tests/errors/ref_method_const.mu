// A mutable `ref` method parameter never binds a readonly place.
unit ref_method_const;
struct S {
    void bump(ref int x) { x = x + 1; }
}
int main() { S s = {}; const int k = 1; s.bump(k); return 0; }
