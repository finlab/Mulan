// slice_zero_init.mu — `new T[n]` zeroes the element block (SPEC 3.3), and `length(axis)` works at every rank
unit slice_zero_init;

extern int puts(const char* s);

struct P { int x; int y; }

class Node {
public:
    int v;
}

int main() {
    // scalars are zeroed
    int[] a = new int[8];
    for i in 0..8 {
        if a[i] != 0 { puts("int not zeroed"); return 1; }
    }

    // a reference element starts as a null handle: releasing the array must not touch anything
    Node[] ns = new Node[4];
    for i in 0..4 {
        if ns[i] != null { puts("handle not zeroed"); return 2; }
    }
    ns[2] = new Node();
    ns[2].v = 7;
    if ns[2].v != 7 { puts("handle write"); return 3; }

    // structs are zeroed field by field
    P[] ps = new P[3];
    if ps[1].x != 0 or ps[1].y != 0 { puts("struct not zeroed"); return 4; }

    // length(): the axis form is also valid at rank 1
    int[,] m = new int[3,5];
    if m.length() != 3 or m.length(0) != 3 or m.length(1) != 5 { puts("length"); return 5; }
    if a.length(0) != 8 { puts("length(0) at rank 1"); return 6; }

    // multidimensional elements are zeroed as one contiguous block
    for i in 0..3 {
        for j in 0..5 {
            if m[i,j] != 0 { puts("2d not zeroed"); return 7; }
        }
    }

    puts("slice zero init ok");
    return 0;
}
