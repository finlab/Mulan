unit contract_body_write;

interface IRead<T> {
    isize length();
    T operator[](Self c, isize i);
}

// Checked at the definition, not per instantiation: the contract grants a *value* subscript, so
// writing through it is rejected even though some containers would return a borrow.
void fill<T: IRead<int>>(T c) {
    for i in 0..c.length() {
        c[i] = 5;
    }
}

int main() {
    return 0;
}
