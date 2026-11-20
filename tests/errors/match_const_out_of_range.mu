unit match_const_out_of_range;
int f(byte b) {
    match b {
        300: { return 1; }
        else: { return 0; }
    }
}
int main() { return f(1); }

