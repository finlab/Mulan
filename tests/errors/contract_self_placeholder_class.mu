// The class variant of contract_self_placeholder: the contract's parameter slot is `Self`, so the
// implementor spells its own type — spelling the interface reads as the same signature but cannot match.
unit contract_self_placeholder_class;

interface IPair {
    bool same(Self o);
}

class Box : IPair {
    override bool same(IPair o) { return false; }
}

int main() {
    Box b = null;
    return b == null ? 0 : 1;
}
