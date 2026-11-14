unit dead_code;

extern int puts(const char* s);

public int unused_public(int x) { return x + 1; }

int unused_private(int x) { return x + 2; }

class Unused {
    int v;
    Unused() { v = 1; }
    ~Unused() { puts("never"); }
    int get() { return v; }
}

struct S {
    int v;
    int unused_method() { return 3; }
}

int main() {
    puts("hi");
    return 0;
}
