// A fixed-size array does not break the by-value cycle: `A` contains an `A` by value however it is
// wrapped, so any array length is infinitely large.
unit rec_array_self;

struct A { A[1] a; }

int main() { return 0; }
