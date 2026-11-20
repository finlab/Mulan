unit match_const_empty_range;
int f(int x) {
    match x {
        5..5: { return 1; }
        else: { return 0; }
    }
}
int main() { return f(1); }

