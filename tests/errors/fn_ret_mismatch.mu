unit fn_ret_mismatch;
int main() {
    auto f = (int x) => { if (x > 0) { return 1; } return 2 as long; };
    return 0;
}
