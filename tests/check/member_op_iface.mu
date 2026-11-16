unit member_op_iface;

interface IAddable { Self operator+(Self a, Self b); }

struct M {
    int v;
    M operator+(M o) { return M(v + o.v); }
}

// The body may only use what the contract grants: `+` (not the struct's field `v`).
T twice<T: IAddable>(T a) { return a + a; }

int main() {
    M x = {2};
    M y = twice<M>(x);
    return y.v - 4;
}
