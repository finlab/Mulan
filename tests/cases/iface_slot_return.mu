unit iface_slot_return;

// Two interfaces with the same method name and parameters but different return types are different
// itable slots: the slot key and the slot -> implementation match include the return type, and each
// thunk uses its own slot's return type. Overload resolution inside a class still ignores it.

use std.cstdio;

interface A {
    bool next();
}

interface B {
    int next();
}

class CA : A {
    override bool next() { return false; }
}

class CB : B {
    override int next() { return 7; }
}

int main() {
    CA a = new CA();
    CB b = new CB();
    int n = b.next();
    bool f = a.next();
    printf("%d %d\n", n, f ? 1 : 0);
    return n == 7 and not f ? 0 : 1;
}
