unit const_member_method_name;

struct S {
    const int X = 1;
    int X() { return 2; }
}

int main() { return 0; }
