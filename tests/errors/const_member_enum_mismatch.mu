unit const_member_enum_mismatch;

enum Color { Red, Green }

struct S {
    const Color C = 1;
}

int main() { return 0; }
