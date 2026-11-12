unit fn_capture;
int main() {
    int base = 1;
    auto f = (int x) => x + base;
    return f(1);
}
