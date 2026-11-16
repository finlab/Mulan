unit c_abi_dtor_return;

struct Owned {
    int* p;
    Owned() { p = null; }
    Owned(move Owned o) { p = o.p; o.p = null; }
    ~Owned() { if p != null { free(p); } }
}

export Owned make() {
    Owned o = Owned();
    return move o;
}

int main() {
    return 0;
}
