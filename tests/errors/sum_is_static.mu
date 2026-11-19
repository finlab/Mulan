unit sum_is_static;
variant O {
    A int;
    B;
}

int main() {
    int x = 3;
    // `int` has no runtime discrimination: `is` would be a constant
    bool e = x is int;
    return 0;
}
