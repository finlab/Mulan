// A length whose byte count overflows aborts like a failed allocation (SPEC 6.6) instead of
// handing out an under-sized block while the header advertises the huge length.
// 2^62 elements of 4 bytes = 2^64, which does not fit in usize.
unit alloc_overflow;
int main() {
    ulong n = 0x4000000000000000;
    int[] a = new int[n];
    return a.length() as int;
}
