unit const_member_compound_assign;

struct S {
    const int X = 1;

    int bare() {
        X += 1;
        return X;
    }
}

int main() {
    S.X += 1;
    S s;
    return s.bare();
}
