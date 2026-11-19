unit para_captured_method_read;

// A `const` method on a value captured by a `para` section is fine (read-only, no race).

struct S {
    int v;
    int read() const { return v; }
}

int main() {
    S s = {};
    para { { if s.read() != 0 { } } { if s.read() != 0 { } } }
    return 0;
}
