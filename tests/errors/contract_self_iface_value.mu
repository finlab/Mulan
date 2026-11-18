// A member that mentions `Self` has no itable slot (its parameter/return type differs per implementor),
// so it cannot be dispatched through an interface value: call it on a concrete type (SPEC 3.15.6).
unit contract_self_iface_value;

interface IClone {
    Self dup();
    int tag();
}

class Box : IClone {
    override Box dup() { return this; }
    override int tag() { return 1; }
}

int main() {
    Box b = new Box();
    IClone c = b;
    IClone d = c.dup();
    return d.tag();
}
