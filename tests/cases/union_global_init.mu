// union_global_init — a named aggregate initializer for a global union writes the selected member
// through the LLVM carrier field (#246).
unit union_global_init;

extern int puts(const char* s);

union U {
    int a;
    int b;
}

U g = { b = 42 };

int main() {
    if g.b != 42 { puts("union init"); return 1; }
    puts("union global init ok");
    return 0;
}
