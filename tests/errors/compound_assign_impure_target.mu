unit compound_assign_impure_target;

struct V {
    int x;
    V operator+(V o) { return V(x + o.x); }
}

V g;

ref V pick() { return g; }

int main() {
    V b = {1};
    pick() += b;
    return 0;
}
