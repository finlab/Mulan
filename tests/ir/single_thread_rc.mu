unit single_thread_rc;

// `--single-thread` must remove every atomic from the generated code: the reference count becomes a
// plain load/sub/store, the acquire fence disappears, and the weak-registry spin lock is skipped
// (only this thread can touch the registry). `putchar` in the destructor keeps the allocation and the
// release path alive through optimization; `atomic_rc` is the same program in the default mode.

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
