// `nomove` forbids moving the value, not using it: it can be constructed in place, passed by `ref`,
// and its fields read and written.
unit move_nomove_ok;

nomove struct Node { int v; }

void bump(ref Node n) { n.v += 1; }

int main() {
    Node n;
    n.v = 41;
    bump(n);
    return n.v == 42 ? 0 : 1;
}
