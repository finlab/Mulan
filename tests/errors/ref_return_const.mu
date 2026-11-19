unit rr3;

struct P {
    int x;
}

// a `const ref` borrow cannot be returned as a mutable `ref`
ref int bad(const ref P p) {
    return p.x;
}

int main() {
    return 0;
}
