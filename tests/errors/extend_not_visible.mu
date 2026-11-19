// `extend IBase with IMore;` lives in `extlib.only`. This unit sees the type (through the interface
// unit) but not the extension, so the method exists yet must not be callable here (SPEC 3.18).
unit extend_not_visible;

use extlib.base;
use extlib.helper;

struct S : IBase {
    public int v;
    override int base() { return v; }
}

int main() {
    S s;
    return s.squared();
}
