unit iface_mutable_readonly;

// a value-returning subscript cannot serve the read-write contract: the borrow is what makes an
// element writable, and a copied element has no storage to write back to
struct Ro {
    int[] d;

    isize length() { return d.length(); }
    int operator[](isize i) { return d[i]; }
}

void fill<T: IIndexMut<int>>(T c, int v) {
    for ref x in c { x = v; }
}

int main() {
    Ro r = {};
    r.d = new int[2];
    fill<Ro>(r, 1);
    return 0;
}
