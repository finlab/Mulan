// `int` is 32 bits and addition wraps (SPEC 8.2), so `MAX + 1` is negative and cannot be an array
// length. Folding it in 64 bits accepted a 2^31-element array (#76).
unit array_len_int_overflow;

const int MAX = 2147483647;

int[MAX + 1] values;

int main() { return 0; }
