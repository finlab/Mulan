// `Self` in a plain interface method denotes the implementor (SPEC 3.15.6): the implementation spells
// its own type, and the `:` declaration, the direct call and the generic constraint agree.
unit contract_self_nominal;
use std.cstdio;

interface IPair {
    bool same(Self o);
}

struct S : IPair {
    public int n;
    override bool same(S o) { return n == o.n; }
}

class C : IPair {
    public int n;
    override bool same(C o) { return this.n == o.n; }
}

bool both<T: IPair>(T a, T b) { return a.same(b); }

int main() {
    S s1 = { n = 4 };
    S s2 = { n = 4 };
    if not both<S>(s1, s2) { printf("struct constraint failed\n"); return 1; }
    C c1 = new C();
    C c2 = new C();
    c1.n = 7;
    c2.n = 7;
    if not both<C>(c1, c2) { printf("class constraint failed\n"); return 2; }
    if not c1.same(c2) { printf("direct call failed\n"); return 3; }
    if not c1.same(c1) { printf("identity compare failed\n"); return 4; }
    printf("self nominal ok\n");
    return 0;
}
