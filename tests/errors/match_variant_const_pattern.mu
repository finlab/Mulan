// A variant is matched by alternative name; a constant pattern is for an integer, `char` or enum.
unit match_variant_const_pattern;

variant V {
    A int;
    B;
}

int main() {
    V v = A(1);
    match v {
        1: { }
        else: { }
    }
    return 0;
}
