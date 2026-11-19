unit promote_missing_field;
struct P { int v; }
class Box {
    public Box(P value) { _value = move value; }
    promote _missing;
    private P _value;
}
int main() { return 0; }
