unit promote_inherited_field;
struct P {
    public int get() { return 1; }
}
class Base {
    public P p;
    public Base(P x) { p = move x; }
}
class Derived : Base {
    public Derived(P x) : Base(x) { }
    promote p;
}
int main() { return 0; }
