unit slice_ptr_temp;

int[] make() {
    return new int[2];
}

int main() {
    int* p = make().ptr();
    return 0;
}
