unit swap_weak_field;

// The weak-reference registry is keyed by (target, field address), so exchanging the bytes of two
// weak fields would leave both registrations attached to the wrong holder (#29).
class Node {
    weak Node next;
    Node() { next = null; }
}

int main() {
    Node a = new Node();
    Node b = new Node();
    swap(a.next, b.next);
    return 0;
}
