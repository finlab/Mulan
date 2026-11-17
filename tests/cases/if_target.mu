unit if_target;
use comptime;

extern int putchar(int c);

// The `std.target` constants are ordinary values: the switch is checked for coverage.
int classify() {
    switch os {
        Windows: { return 1; }
        Linux: { return 2; }
        MacOS: { return 3; }
    }
}

int addr_bits() {
    switch arch {
        X86_64: { return 64; }
        AArch64: { return 64; }
    }
}

int main() {
    if classify() < 1 or classify() > 3 {
        return 1;
    }
    if addr_bits() != ptr_bits {
        return 2;
    }
    if endian == Endian.Little and ptr_bits != 64 {
        return 3;
    }
    putchar('o');
    putchar('k');
    putchar('\n');
    return 0;
}
