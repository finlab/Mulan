// Delegating constructors form a chain that must not loop (SPEC 3.12).
unit ctor_delegate_cycle;
class Cfg { public int id; Cfg() : Cfg(1) { } Cfg(int i) : Cfg() { } }
int main() { return 0; }
