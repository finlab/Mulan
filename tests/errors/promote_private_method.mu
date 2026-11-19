unit promote_private_method;
class P {
    private int hidden() { return 1; }
}
class Box {
    private P _value;
    public Box(P value) { _value = value; }
    promote _value(hidden);
}
int main() { return 0; }
