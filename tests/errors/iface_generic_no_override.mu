unit iface_generic_no_override;
use std.interfaces;

class Counter : IIterator<int> {
    int i;
    Counter() { i = 0; }
    override bool next() {
        i = i + 1;
        return i <= 2;
    }
    override int current() { return i; }
}

// A class still has to mark interface implementations `override` (unlike a struct).
class Bad : IEnumerable<int> {
    IIterator<int> iterator() { return new Counter(); }
}

int main() {
    return 0;
}
