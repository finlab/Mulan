unit single_thread_rc;

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

