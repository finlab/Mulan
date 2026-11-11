unit const_member_assign;

struct S {
    const int X = 1;
}

int main() {
    S.X = 2;
    return 0;
}
