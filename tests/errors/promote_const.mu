unit promote_const;
struct P {
    int v;
    public int get() { return v; }
}
class Box {
    private P _value;
    public Box(P value) { _value = move value; }
    promote _value;
}
int main() {
    P p = {};
    const Box b = new Box(p);
    return b.get();
}
