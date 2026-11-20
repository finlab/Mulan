// numeric_literals — `_` is accepted between digits in decimal/hex/binary/float literals.
unit numeric_literals;

extern int puts(const char* s);

int main() {
    int a = 1_000;
    int b = 0x1_ff;
    int c = 0b10_01;
    double d = 1_2.5_0;
    if a != 1000 or b != 511 or c != 9 { puts("separators"); return 1; }
    if d != 12.5 { puts("float"); return 2; }
    puts("numeric literals ok");
    return 0;
}
