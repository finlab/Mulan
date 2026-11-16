// slice_uninit.mu — `uninit new T[n]`: a trivial element block with no zeroing cost
unit slice_uninit;

extern int puts(const char* s);

struct P { int x; float y; }

int main() {
    // every element is written before it is read (writing is what `uninit` is for)
    int[] a = uninit new int[16];
    for i in 0..16 {
        a[i] = i * 2;
    }
    for i in 0..16 {
        if a[i] != i * 2 { puts("int uninit"); return 1; }
    }

    // the header is valid: length and bounds checking work as usual
    if a.length() != 16 { puts("length"); return 2; }
    try {
        int bad = a[16];
        puts("bounds not checked");
        return 3;
    } catch (e: BoundsError) {
        if e.index != 16 or e.length != 16 { puts("bounds fields"); return 4; }
    }

    // a multidimensional block is one contiguous allocation
    P[] ps = uninit new P[4];
    for i in 0..4 {
        ps[i].x = i;
        ps[i].y = 1.5;
    }
    if ps[3].x != 3 or ps[3].y != 1.5 { puts("struct uninit"); return 5; }

    puts("slice uninit ok");
    return 0;
}
