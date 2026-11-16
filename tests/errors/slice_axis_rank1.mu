unit slice_axis_rank1;

int main() {
    // a rank-1 sequence has axis 0 only; `length()` is the same as `length(0)`
    int[] a = new int[4];
    return a.length(1) as int;
}
