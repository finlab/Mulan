unit delegate_this_class_ref;
type Bump = delegate () => void;

class K {
    public int n;
    // `this` is already a reference for a class: `[ref this]` has nothing to borrow
    Bump inc() { return [ref this] () => { n = n + 1; }; }
}

int main() {
    return 0;
}
