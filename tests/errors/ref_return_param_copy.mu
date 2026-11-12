unit rr2;

struct Small {
    int[4] v;
}

// by-value receiver: the returned borrow points to the parameter copy (inline storage) and dangles
ref int bad(Small s, usize i) {
    return s.v[i];
}

int main() {
    return 0;
}
