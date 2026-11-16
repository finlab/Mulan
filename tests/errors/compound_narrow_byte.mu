unit compound_narrow_byte;
// Byte arithmetic promotes to `int`, so `b += 1000` is a narrowing store, like `b = b + 1000`.
int main() {
    byte b = 10;
    b += 1000;
    return 0;
}
