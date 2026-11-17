// if_generic_body.mu — a generic template's body `#if` is expanded at its own source position, so it
// sees the constants declared before it (a `sizeof` constant too) and the field order is kept.
unit if_generic_body;

extern int puts(const char* s);

struct P { int x; int y; }

const int W = sizeof(P) as int;

struct Box<T> {
    T v;
#if W == 8 {
    int extra;
}
#else {
    int other;
}
}

int main() {
    Box<int> b;
    b.v = 1;
    b.extra = 2;
    if sizeof(Box<int>) != 8 { puts("generic body layout wrong"); return 1; }
    puts("generic body ok");
    return 0;
}
