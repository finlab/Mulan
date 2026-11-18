// Every prologue item names the base class, the type itself, or a field of this type (SPEC 3.12).
unit ctor_unknown_initializer;
class Cfg { public int id; Cfg() : nope(1) { } }
int main() { return 0; }
