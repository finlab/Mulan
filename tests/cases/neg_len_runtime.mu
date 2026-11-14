// A computed negative length aborts (SPEC 6.6: an allocation that cannot be satisfied aborts)
// instead of wrapping the byte count and handing out a heap buffer smaller than its advertised
// length. Nothing is printed, so the expectation is only the exit code (abort() is 3 on Windows).
unit neg_len_runtime;
int main() {
    int n = -1;
    int[] a = new int[n];
    // unreachable: the allocation above aborts
    return a.length() as int;
}
