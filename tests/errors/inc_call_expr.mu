unit inc_call_expr;
int f(int x) { return x; }
int main() {
    int i = 0;
    f(i++);
    return 0;
}
