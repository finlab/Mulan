unit member_op_arity;
struct S {
    int v;
    int operator+(int a, int b) { return a + b; }
}
int main() { return 0; }
