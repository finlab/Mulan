unit ref_generic_rc;

struct Box {
    int[] data;
}

// A `ref` parameter may not borrow a value that owns something: the borrow would alias storage whose
// reference count the callee does not own. The rule is checked for concrete types; a type parameter
// only becomes concrete at instantiation, so the same check has to run there too — without it this
// compiled and then corrupted the heap at run time.
void touch<T>(ref T c) {
}

int main() {
    Box b = {};
    touch<Box>(b);
    return 0;
}
