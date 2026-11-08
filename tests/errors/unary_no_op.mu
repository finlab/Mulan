unit unary_no_op; struct P { int x; } int main() { P p = {1}; P q = -p; return q.x; }
