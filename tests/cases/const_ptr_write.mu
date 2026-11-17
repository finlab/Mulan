// const_ptr_write.mu — a read-only pointer still writes through: `int* const p` may not be retargeted
// (that is `const_ptr_retarget`), but `*p = v` is a write to the pointee (SPEC 3.7).
unit const_ptr_write;

extern int puts(const char* s);

int main() {
    int x = 5;
    int* const p = &x;
    *p = 7;
    if x != 7 { puts("pointee write failed"); return 1; }
    puts("const pointer write ok");
    return 0;
}
