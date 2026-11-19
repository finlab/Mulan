unit sum_self_payload;
// A payload stored by value cannot be the sum itself: `Holds(H)` has no finite layout. This used to
// be accepted and then killed the compiler in the layout recursion.
variant Holder {
    Holds Holder;
    None;
}

int main() {
    Holder h = None;
    if h is None {
        return 0;
    }
    return 1;
}
