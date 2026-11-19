unit promote_whitelist_unknown;
struct P {
    public int get() { return 1; }
}
class Box {
    private P _value;
    public Box(P value) { _value = move value; }
    promote _value(nope);
}
int main() { return 0; }
