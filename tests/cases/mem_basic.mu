// mem_basic — `std.mem`: element-wise copy / fill / compare / align_up on a managed sequence.
// Element assignment goes through the language, so a reference element is retained and released
// exactly as a hand-written `dst[i] = src[i]` would do.
unit mem_basic;

use std.mem;
use std.string;
use std.cstdio;

int main() {
    int[] a = new int[4];
    int[] b = new int[4];
    fill<int>(b, 3);
    copy<int>(a, b);
    printf("%d %d %d %d\n", a[0], a[1], a[2], a[3]);

    // a short destination throws the ordinary bounds error at the first out-of-range index
    int[] small = new int[2];
    int caught = 0;
    try {
        copy<int>(small, a);
    } catch (e: BoundsError) {
        caught = 1;
    }
    printf("%d\n", caught);

    // lexicographic comparison, including the prefix rule
    int[] c = new int[2];
    c[0] = 1; c[1] = 9;
    int[] d = new int[2];
    d[0] = 1; d[1] = 2;
    int[] prefix = new int[1];
    prefix[0] = 1;
    printf("%d %d %d %d\n", compare<int>(c, d), compare<int>(d, c), compare<int>(c, c),
           compare<int>(prefix, c));

    // string elements: copy shares the payload (one reference), not the byte block
    string[] s1 = new string[2];
    s1[0] = string("alpha");
    s1[1] = string("beta");
    string[] s2 = new string[2];
    copy<string>(s2, s1);
    printf("%s %s\n", s2[0].cstr(), s2[1].cstr());

    printf("%d %d %d %d\n", align_up(5, 4), align_up(8, 8), align_up(0, 16), align_up(17, 16));

    int bad = 0;
    try {
        align_up(5, 3);
    } catch (e: MemError) {
        bad = 1;
    }
    printf("%d\n", bad);
    return 0;
}
