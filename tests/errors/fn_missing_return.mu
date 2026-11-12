unit fn_missing_return;
int main() {
    auto f = (int x) => { if (x > 0) { return 1; } };
    return 0;
}
