unit switch_no_colon;
int f(int x) {
    switch x {
        1 { return 1; }
    }
}
int main() { return f(1); }

