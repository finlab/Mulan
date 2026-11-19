unit type_extend_rejected;
interface I {
    void f();
}
struct S {
    void f() { }
}
type S += I;
int main() { return 0; }
