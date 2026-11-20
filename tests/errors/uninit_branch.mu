// Only one branch assigns, so the read after the `if` is an error (SPEC 4).
unit uninit_branch;
int main() {
    int y;
    if 1 > 0 { y = 1; }
    return y;
}
