unit const_expr_circular;

const int A = B;
const int B = A;

int main() { return 0; }
