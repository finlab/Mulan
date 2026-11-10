unit inc_paren_expr;
int main() {
    int i = 0;
    int* p = &i;
    int x = *(p++);
    return x;
}