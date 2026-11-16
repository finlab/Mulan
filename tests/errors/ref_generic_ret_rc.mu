unit ref_generic_ret_rc;

// `ref T` may only borrow a plain value. A type parameter is not concrete when the template is
// parsed, so the check has to run again at instantiation — otherwise a generic accessor would
// hand out a borrow of a reference-counted field (retain/release bypassed).
class Node {
    int n;
}

struct Cell<T> {
    T v;

    ref T at() { return v; }
}

int main() {
    Cell<Node> c;
    return 0;
}
