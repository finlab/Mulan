unit atomic_rc;

// Default mode, same program as `single_thread_rc`: reference counting is atomic (relaxed add,
// release sub + acquire fence on the final release, SPEC 3.17 / 8.3) and the weak registry is
// guarded by its spin lock (`atomicrmw xchg`).

extern int putchar(int c);

class Node {
    int v;
    weak Node next;
    Node(int x) { v = x; next = null; }
    ~Node() { putchar(10); }
}

Node last;

int main(int argc, char** argv) {
    Node keep = new Node(argc);
    int n = argc + 2;
    for i in 0..n {
        Node p = new Node(i);
        keep.next = p;
        last = p;
    }
    last = null;
    keep = null;
    return 0;
}
