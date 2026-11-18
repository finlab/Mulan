// `sizeof(S)` is evaluated during collection, which resolves S's fields early; an array bound inside S
// must still see the unit's earlier `const`s (#82).
unit sizeof_struct_const_bound;

const int N = 4;
struct S { int[N] values; }
const int SIZE = sizeof(S) as int;

int main() { return SIZE == 16 ? 0 : 1; }
