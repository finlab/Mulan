unit match_two_else;
int f(int x) {
    match x {
        1: { return 1; }
        else: { return 0; }
        else: { return 2; }
    }
}
int main() { return f(1); }

