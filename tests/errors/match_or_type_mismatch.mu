unit match_or_type_mismatch;

variant V {
    A int;
    B const char*;
}

int f(V v) {
    return match v {
        A(x) | B(x): 1;
    };
}

int main() { return f(A(1)); }
