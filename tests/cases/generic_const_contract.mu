// generic_const_contract — a `const` contract is satisfied by a `const` implementation in the
// structural constraint path.
unit generic_const_contract;

extern int puts(const char* s);

interface Sized {
    int size() const;
}

struct Good {
    int n;
    Good() { n = 4; }
    int size() const { return n; }
}

int probe<T: Sized>(T x) { return x.size(); }

int main() {
    Good g = {};
    if probe(g) != 4 { puts("probe"); return 1; }
    puts("generic const contract ok");
    return 0;
}
