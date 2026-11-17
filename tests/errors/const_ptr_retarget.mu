// A top-level `const` pointer is read-only: `p = &y` may not retarget it (only `*p` may be written), SPEC 3.7.
unit const_ptr_retarget;

int main() {
    int x = 5;
    int y = 6;
    int* const p = &x;
    p = &y;
    return 0;
}
