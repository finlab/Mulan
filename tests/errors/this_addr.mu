unit this_addr;
extern int puts(const char* s);
struct S { int v; int* addr() { return &this; } }
int main() { puts("unreachable"); return 0; }
