// packed structs cannot directly contain a dynamic array (unaligned pointer access)
unit packed_slice_field;

packed struct Bad {
    byte tag;
    int[] xs;
}

int main() { return 0; }
