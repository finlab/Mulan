unit const_member_addr;

struct S {
    const int X = 1;
}

int main() {
    int* p = &S.X;
    return 0;
}
