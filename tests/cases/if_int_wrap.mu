// `const int MAX = 2147483647; MAX + 1` wraps to the 32-bit minimum (SPEC 8.2), so the condition is
// true. Folding it in 64-bit made the same program take the other arm (#71).
unit if_int_wrap;

extern int puts(const char* s);

const int MAX = 2147483647;
const int MIN = -2147483647 - 1;

#if MAX + 1 == MIN {
int main() { puts("int wrap"); return 0; }
}
#else {
int main() { puts("int no-wrap"); return 1; }
}
