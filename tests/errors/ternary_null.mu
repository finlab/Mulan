unit ternary_null;
int main() {
    int a = 1;
    int* p = a > 0 ? null : a;
    return 0;
}
