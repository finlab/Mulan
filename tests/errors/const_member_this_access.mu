unit const_member_this_access;

struct S {
    const int X = 1;

    int f() {
        return this.X;
    }
}

int main() {
    S s;
    return s.f();
}
