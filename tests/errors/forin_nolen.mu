unit forin_nolen;

// missing length(): cannot be iterated with for-in
struct NoLen {
    int x;
}

int operator[](NoLen v, usize i) { return v.x; }

int main() {
    NoLen n = {};
    for x in n { }
    return 0;
}
