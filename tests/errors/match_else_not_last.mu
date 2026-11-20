unit match_else_not_last;
int f(int x) {
    match x {
        else: { return 0; }
        1: { return 1; }
    }
}
int main() { return f(1); }

