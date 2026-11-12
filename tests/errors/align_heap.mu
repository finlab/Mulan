// align(N) / packed structs cannot be dynamic array elements: the data area follows a 24-byte header and is at most 8-byte aligned
unit align_heap;

align(16) struct Wide {
    int x;
    int y;
}

int main() {
    Wide[] ws = new Wide[2];
    return 0;
}
