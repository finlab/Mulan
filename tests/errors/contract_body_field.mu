unit contract_body_field;

interface IAdd {
    Self operator+(Self a, Self b);
}

struct M {
    int v;
    M operator+(M o) { return M(v + o.v); }
}

// The contract grants `+` only; a field of some satisfying type is not part of it.
int probe<T: IAdd>(T a) {
    return a.v;
}

int main() {
    return 0;
}
