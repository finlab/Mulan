unit fn_addr_of_func;
int add(int a, int b) { return a + b; }
int main() {
    auto f = &add;
    return 0;
}
