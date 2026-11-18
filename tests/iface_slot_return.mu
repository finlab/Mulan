unit iface_slot_return;

// PROBE (not an assertion, kept so the repro does not get lost): two interfaces with the same method
// name and parameters but different return types collapse into one itable slot, because the slot key and
// the implementation match ignore the return type. Expected: builds and runs; today it reports
// "LLVM backend: generated module is invalid: Function return type does not match operand type of
// return inst" (agents/TODO.md, codegen/接口).
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
    override int next() { return 1; }
}

int main() {
    CA a = new CA();
    CB b = new CB();
    return b.next();
}
