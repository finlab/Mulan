// contract_witness_ref_ret — a `ref` / `ref const` interface return keeps its referent type in a
// generic constrained by that interface: `x.at()` is the field, not the receiver.
unit contract_witness_ref_ret;

use std.cstdio;

interface IRef {
    ref int at();
    ref const int cat() const;
}

class Holder : IRef {
    public int v;
    Holder(int x) { v = x; }
    override ref int at() { return v; }
    override ref const int cat() const { return v; }
}

struct SBox : IRef {
    public int v;
    SBox(int x) { v = x; }
    override ref int at() { return v; }
    override ref const int cat() const { return v; }
}

int poke<T: IRef>(T x) {
    x.at() = 42;
    return x.cat();
}

int poke_ref<T: IRef>(ref T x) {
    x.at() = 42;
    return x.cat();
}

int main() {
    Holder h = new Holder(1);
    if poke<Holder>(h) != 42 { printf("class ref\n"); return 1; }
    if h.v != 42 { printf("class write-through\n"); return 2; }

    SBox s = SBox(5);
    if poke<SBox>(s) != 42 { printf("struct ref\n"); return 3; }
    if s.v != 5 { printf("struct by-value copy written\n"); return 4; }  // a struct parameter is a copy

    // through a `ref` parameter the write reaches the caller's object
    if poke_ref<SBox>(s) != 42 { printf("struct ref param\n"); return 5; }
    if s.v != 42 { printf("struct ref write-through\n"); return 6; }

    printf("witness ref ret ok\n");
    return 0;
}
