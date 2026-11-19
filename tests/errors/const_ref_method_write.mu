unit const_ref_method_write;

// A `const ref` receiver is read-only: calling a non-const method on it must be rejected (the
// read-only counterpart, `read() const`, is fine).

struct S {
    int v;
    void poke() { v = 1; }
    int read() const { return v; }
}

int use_write(const ref S s) { s.poke(); return s.read(); }

int main() {
    S s = {};
    return use_write(s);
}
