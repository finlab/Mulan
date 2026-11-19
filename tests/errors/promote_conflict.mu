unit promote_conflict;
struct P {
    public int get() { return 1; }
}
class Box {
    private P a;
    private P b;
    public Box(P x) { a = move x; b = move x; }
    promote a;
    promote b;
}
int main() { Box x = new Box(P()); return x.get(); }
