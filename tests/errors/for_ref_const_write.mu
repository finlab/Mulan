unit for_ref_const_write;

int main() {
    int[3] a = {1, 2, 3};
    const int[] rs = a;
    for ref const v in rs { v = 2; }
    return 0;
}
