// A `ref` method parameter borrows the caller's storage, so a temporary cannot be passed.
unit ref_method_temp;
struct S {
    int v;
    void bump(ref int x) { x = x + v; }
}
int main() { S s = {1}; s.bump(5); return 0; }
