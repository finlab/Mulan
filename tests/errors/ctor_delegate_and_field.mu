// A delegating constructor hands the whole construction to the target: it cannot initialize fields itself
// (SPEC 3.12).
unit ctor_delegate_and_field;
class Cfg { public int id; Cfg(int i) : id(i) { } Cfg() : Cfg(1), id(2) { } }
int main() { return 0; }
