// A union member that reaches back to the union through a struct: this used to kill the compiler with
// a stack overflow in the layout recursion, because the struct pass only checked the depth limit.
unit rec_union_struct;

union U { S s; int x; }
struct S { U u; }

int main() { return 0; }
