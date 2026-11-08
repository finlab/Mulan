unit cast_ptr_slice;
int main() { int* p = alloc(8) as int*; int[] s = p as int[]; free(p); return 0; }
