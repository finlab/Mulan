unit forin_nolen;

// missing length(): cannot be iterated with for-in
struct NoLen {
    int x;
    int operator[](isize i) { return x; }
}

int main() {
    NoLen n = {};
    for x in n { }
    return 0;
}
