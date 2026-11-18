// `3.9 as int` is a compile-time integer constant: the `as` performs the truncation (#77).
unit array_len_float_cast;

int[3.9 as int] values;

const int N = 2.7 as int;
int[N] more;

int main() { return 0; }
