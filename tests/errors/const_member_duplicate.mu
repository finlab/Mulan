unit const_member_duplicate;

struct S {
    const int X = 1;
    const int X = 2;
}

int main() { return 0; }
