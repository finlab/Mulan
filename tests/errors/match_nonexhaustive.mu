unit match_nonexhaustive;
variant V {
    A int;
    B;
}
int main() {
    V v = A(1);
    int r = match v {
        A(n): n;
    };
    return r;
}
