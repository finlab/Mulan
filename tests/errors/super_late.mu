// A `super(...)` call that is not the first statement used to be a "must be first" error; now the base
// constructor is a prologue item, so a `super(...)` statement is not a statement at all (SPEC 3.12).
unit super_late;
class A { A() { } }
class B : A { B() { int x = 1; super(); } }
int main() { return 0; }
