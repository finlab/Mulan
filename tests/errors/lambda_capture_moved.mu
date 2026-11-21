unit lambda_capture_moved;
struct B { int n; B() { n = 1; } }
type Get = delegate () => int;
int main() {
    B b = {};
    B c = move b;
    Get f = [b]() => b.n;
    return f();
}
