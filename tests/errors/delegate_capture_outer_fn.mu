unit delegate_capture_outer_fn;
type Get = delegate () => int;

int main() {
    int y = 2;
    // the inner lambda tries to capture `y` from `main`, skipping the enclosing (empty) lambda:
    // the outer lambda must capture it first
    Get g = [] () => ([y] (int a) => a + y)(1);
    return g();
}
