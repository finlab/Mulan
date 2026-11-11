unit const_member_circular;

struct S {
    const int A = B;
    const int B = A;
}

int main() { return 0; }
