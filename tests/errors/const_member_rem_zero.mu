unit const_member_rem_zero;

struct S {
    const int X = 1 % 0;
}

int main() { return 0; }
