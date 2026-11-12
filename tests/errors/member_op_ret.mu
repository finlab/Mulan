unit member_op_ret;
struct S {
    int v;
    int operator==(S o) { return 1; }
}
int main() { return 0; }
