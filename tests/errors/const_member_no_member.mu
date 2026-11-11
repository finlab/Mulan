unit const_member_no_member;

struct S {
    const int X = 1;
}

int main() {
    return S.Y;
}
