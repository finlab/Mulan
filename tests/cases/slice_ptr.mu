// slice_ptr.mu — `T[].ptr()`: the data pointer of a managed sequence (C interop)
unit slice_ptr;

extern int puts(const char* s);

int main() {
    int[] b = new int[3];
    int* p = b.ptr();
    p[0] = 5;
    p[1] = 6;
    p[2] = 7;
    if b[1] != 6 { puts("ptr write failed"); return 1; }
    if p[2] != 7 { puts("ptr read failed"); return 1; }

    int[] empty = {};
    if empty.ptr() != null { puts("empty ptr should be null"); return 1; }

    // a multidimensional array exposes the same contiguous block
    int[,] m = new int[2,3];
    int* mp = m.ptr();
    m[1,2] = 9;
    if mp[5] != 9 { puts("2d ptr failed"); return 1; }

    puts("slice ptr ok");
    return 0;
}
