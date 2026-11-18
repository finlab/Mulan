// The abstract-class diagnostic names the unimplemented interface members.
unit abstract_missing_member;

interface I { int f(); }

class C : I {
public:
    int v;
}

int main() {
    C c = new C();
    return 0;
}
