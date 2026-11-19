// The argument's structure does not match the parameter pattern (SPEC 3.15.3): the diagnostic names
// the argument position and the pattern, so the reader knows where inference stopped.
unit generic_infer_shape;

struct Thing {
    int v;
}

T deref<T>(T* p) { return *p; }

int main() {
    Thing t = {1};
    Thing value = deref(t);
    return value.v;
}
