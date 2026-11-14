// A constant negative dynamic-array length is a compile error (SPEC 6.4: length() is the number
// of valid indices, so a negative length cannot be represented).
unit neg_len;
int main() {
    int[] a = new int[-1];
    int[,] b = new int[-1, 3];
    return 0;
}
