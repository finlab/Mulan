unit switch_ptr_scrut;
int f(int* p) {
    switch p {
        1: { return 1; }
        else: { return 0; }
    }
}
int main() { int a = 1; return f(&a); }

