unit rr1;

ref int bad() {
    int a = 1;
    return a;
}

int main() {
    bad() = 2;
    return 0;
}
