unit match_or_dup;

variant V {
    A int;
    B int;
}

int f(V v) {
    return match v {
        A(x) | A(x): 1;
        B(x): 2;
    };
}

int main() { return f(A(1)); }
