unit atomic_rc;

// Reference counting is atomic by default: the retain at the call and the release at the end of the
// scope must stay atomic, and the final release needs the acquire fence (SPEC 3.17 / 8.3).
// Paired with `single_thread_rc.txt`, which asserts that `--single-thread` drops both.

class Node {
    int v;
    Node(int x) { v = x; }
}

int read(Node n) {
    return n.v;
}

int main() {
    Node a = new Node(1);
    int r = read(a);
    a = null;
    return r == 1 ? 0 : 1;
}
