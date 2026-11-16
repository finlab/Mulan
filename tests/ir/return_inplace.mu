// A struct result is built in the caller's `sret` slot, not in a local temporary that is then
// copied out: the local's storage *is* the hidden pointer, so `return s;` is a handful of stores.
// A regression that materialises the local first shows up as a memcpy on the way out.
unit return_inplace;

public struct Big {
    public isize a;
    public isize b;
    public isize c;
}

export Big make(isize x) {
    Big s;
    s.a = x;
    s.b = x + 1;
    s.c = x + 2;
    return s;
}

export isize sum_after_call(isize x) {
    Big v = make(x);
    return v.a + v.b + v.c;
}

int main() {
    return (sum_after_call(1)) as int;
}
