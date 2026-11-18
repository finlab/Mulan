// Every value type is movable by default (the compiler copies the bits and empties the source);
// `nomove` is the explicit opt-out for a value whose address is part of it.
unit move_not_movable;

nomove struct S { int x; ~S() { } }

int main() { S a; S b = move a; return b.x; }
