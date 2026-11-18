// A `const` method borrows its receiver read-only, so it cannot hand out a mutable `ref T`: the caller
// of a const method must not be able to write a field through the returned borrow (#66).
unit const_method_mutable_ref;

struct Slot { public int v; }

class Holder {
    public Slot s;
    Holder(int x) : s(Slot(x)) { }
    ref int at() const { return s.v; }
}

int main() {
    Holder h = new Holder(1);
    h.at() = 42;
    return h.s.v == 42 ? 0 : 1;
}
