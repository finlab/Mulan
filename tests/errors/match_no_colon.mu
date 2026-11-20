unit match_no_colon;
int f(int x) {
    match x {
        1 { return 1; }
    }
}
int main() { return f(1); }

