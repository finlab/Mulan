unit const_member_not_const;

int y = 3;

struct S {
    const int X = y;
}

int main() { return 0; }
