unit member_op_class;
class C {
    int v;
    int operator+(C o) { return 1; }
}
int main() { return 0; }
