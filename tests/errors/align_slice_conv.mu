// a fixed-size array of align(16) structs cannot be implicitly converted to a dynamic array (the conversion allocates a data area, at most 8-byte aligned)
unit align_slice_conv;

align(16) struct Wide {
    int x;
    int y;
}

int main() {
    Wide[2] arr;
    Wide[] ws = arr;
    return 0;
}
