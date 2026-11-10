unit op_neg;
extern int puts(const char* s);

struct Bits {
    int v;
}

Bits operator~(Bits b) {
    Bits r = {~b.v};
    return r;
}

int main() {
    Bits b = {0};
    Bits c = ~b;
    if c.v != -1 { puts("op~ fail"); return 1; }
    puts("op~ ok");
    return 0;
}
