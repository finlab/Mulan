unit const_member_sizeof_strict;

struct S {
    const int C = sizeof(int);
}

const int G = sizeof(int);

int main() {
    return S.C + G;
}
