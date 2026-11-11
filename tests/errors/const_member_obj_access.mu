unit const_member_obj_access;

struct S {
    const int X = 1;
}

int main() {
    S s;
    return s.X;
}
