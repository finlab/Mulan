public unit constrain.order;

// The constraint names a type this unit imports. A caller that does not import `std.interfaces` must
// still be able to instantiate the template: the bound is resolved in the unit that wrote it.
use std.interfaces;

public T smallest<T: IOrder>(T[] s) {
    T best = s[0];
    for i in 1..s.length() {
        if s[i] < best { best = s[i]; }
    }
    return best;
}
