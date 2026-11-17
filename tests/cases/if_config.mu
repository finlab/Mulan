unit if_config;
use comptime;

extern int putchar(int c);

// A conditional field changes the layout: the directive is spliced into the member list.
struct Header {
    int tag;
#if ptr_bits == 64 {
    int pad;
}
}

int expected_size() {
#if ptr_bits == 64 {
    return 8;
}
#else {
    return 4;
}
}

// A condition that is false on every platform: the arm is parsed but never compiled.
int branch_value() {
    int h = 0;
#if os == OS.Windows and os == OS.Linux {
    h = -1;
}
#else {
    h = 5;
}
    return h;
}

void print_int(int v) {
    if v < 0 {
        putchar('-');
        v = -v;
    }
    if v == 0 {
        putchar('0');
        return;
    }
    int[20] d;
    int i = 0;
    while v > 0 {
        d[i] = v % 10;
        v /= 10;
        i += 1;
    }
    while i > 0 {
        i -= 1;
        putchar('0' + d[i]);
    }
}

int main() {
    // layout and the branch condition agree
    print_int(sizeof(Header) as int - expected_size());
    putchar(' ');
    print_int(branch_value());
    putchar('\n');
    if sizeof(Header) as int != expected_size() {
        return 1;
    }
    if branch_value() != 5 {
        return 2;
    }
    return 0;
}
