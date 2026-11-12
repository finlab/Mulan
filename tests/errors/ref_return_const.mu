unit rr3;

struct P {
    int x;
}

// a `ref const` borrow cannot be returned as a mutable `ref`
ref int bad(ref const P p) {
    return p.x;
}

int main() {
    return 0;
}
