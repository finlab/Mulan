unit slice_ptr_field;

int main() {
    // `ptr` is a method: the field form is rejected
    int[] a = new int[2];
    int* p = a.ptr;
    return 0;
}
