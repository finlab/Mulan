// The loop body may not run, so an assignment inside it does not initialize the variable (SPEC 4).
unit uninit_loop;
int main() {
    int z;
    for i in 0..3 { z = i; }
    return z;
}
