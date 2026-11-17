// `MIN / -1` and `MIN % -1` overflow a 64-bit `long`: folding them in the compiler's own C++ would trap
// (SIGFPE), so the evaluator rejects the expression and the array-length check reports it.
unit const_div_overflow;

const long MIN = -9223372036854775807 - 1;

int[MIN / -1] values;

int main() { return 0; }
