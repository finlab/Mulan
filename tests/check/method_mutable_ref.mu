// A non-`const` method may lend a mutable borrow: the receiver is borrowed mutably, so writing through
// the result is allowed (#66).
unit method_mutable_ref;

struct Slot { public int v; }

class Holder {
    public Slot s;
    Holder(int x) : s(Slot(x)) { }
    ref int at() { return s.v; }
}

int main() {
    Holder h = new Holder(1);
    h.at() = 42;
    return h.s.v == 42 ? 0 : 1;
}
