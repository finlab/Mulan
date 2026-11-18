// The implementor must spell its own type in a `Self` slot. Spelling the interface instead makes the
// type satisfy the contract structurally (where `Self` is substituted) but not nominally.
unit contract_self_placeholder;

interface IPair {
    bool same(Self o);
}

struct S : IPair {
    override bool same(IPair o) { return false; }
}

int main() {
    S s;
    return 0;
}
