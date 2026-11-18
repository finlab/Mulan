// A host containing a value with a user destructor is movable by default: the compiler moves it by
// copying the bits and emptying (and not destroying) the source, which is what the host would do in
// its own move constructor.
unit struct_dtor_move_host;

struct S { int v; S() { v = 1; } ~S() { } }
struct O { S s; int tag; }

int main() {
    O a = {};
    O b = move a;
    return b.s.v;
}
