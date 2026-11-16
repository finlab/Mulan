unit slice_ptr_const;

void peek(const int[] a) {
    // a const block yields `const T*`: the pointer must not become a write hole
    int* p = a.ptr();
}

int main() {
    int[] a = new int[2];
    peek(a);
    return 0;
}
