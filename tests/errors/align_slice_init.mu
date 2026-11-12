// aggregate initialization of a slice of align(16) structs also allocates a data area (at most 8-byte aligned)
unit align_slice_init;

align(16) struct Wide {
    int x;
    int y;
}

int main() {
    Wide[] ws = { Wide(1, 2), Wide(3, 4) };
    return 0;
}
