unit match_or_bind_mismatch;

variant V {
    A int;
    B int;
}

int f(V v) {
    return match v {
        A(x) | B(y): x;
    };
}

int main() { return f(A(1)); }
