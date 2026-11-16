unit c_abi_dtor_param;

// A movable struct with a destructor still cannot cross a C ABI boundary by value: the C caller does
// not participate in the ownership protocol the Mulan callee assumes (#30).
struct Owned {
    int* p;
    Owned() { p = null; }
    Owned(move Owned o) { p = o.p; o.p = null; }
    ~Owned() { if p != null { free(p); } }
}

extern void take(Owned o);

int main() {
    return 0;
}
