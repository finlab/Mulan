// contract_iface_default.mu — a *declaring* implementor gets the interface's default methods
// materialized, and those count as members for a structural constraint check too (SPEC 3.15.5).
unit contract_iface_default;

extern int puts(const char* s);

interface IVal {
    int val();
    int twice() { return val() * 2; }
}

struct Item : IVal {
    public int n;
    int val() { return n; }
}

int useit<T: IVal>(T x) { return x.twice(); }

int main() {
    Item i;
    i.n = 21;
    if useit<Item>(i) != 42 { puts("materialized default did not satisfy the constraint"); return 1; }
    puts("iface default contract ok");
    return 0;
}
