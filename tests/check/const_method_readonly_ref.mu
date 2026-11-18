// `ref const T at() const` is the read-only borrow a const method may return; it reads, and writing
// through it is rejected at the call site (#66).
unit const_method_readonly_ref;

struct Slot { public int v; }

class Holder {
    public Slot s;
    Holder(int x) : s(Slot(x)) { }
    ref const int at() const { return s.v; }
}

int main() {
    Holder h = new Holder(7);
    int x = h.at();
    return x == 7 ? 0 : 1;
}
