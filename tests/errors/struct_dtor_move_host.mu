// struct_dtor_move_host — the same rule on the move axis: a host that contains a value with a user
// destructor transfers ownership only through its own move constructor (the compiler's bit move would
// skip it), so `move` must be refused until the host declares one.
unit struct_dtor_move_host;
struct S { int v; S() { v = 1; } ~S() { } }
struct O { S s; int tag; }
int main() {
    O a = {};
    O b = move a;
    return 0;
}
