// A *structurally* satisfying type must provide the interface's default methods itself: a structural
// relation has no place to materialize a default (SPEC 3.15.5, decided 2026-11-18).
unit contract_structural_default;

interface IVal {
    int val();
    int twice() { return val() * 2; }
}

struct Item {
    public int n;
    int val() { return n; }
}

int useit<T: IVal>(T x) { return x.twice(); }

int main() {
    Item i;
    i.n = 21;
    return useit<Item>(i);
}
