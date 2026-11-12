unit fn_overload_no_target;
int over(int a) { return a; }
int over(int a, int b) { return a + b; }
int main() {
    auto f = over;
    return 0;
}
