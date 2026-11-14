// unit_init_assign.mu — a zero-initialized unit-level variable is still cleaned up at exit
//
// `current` has no initializer, so it stays in the data area (null handle), but the exit sequence
// must release whatever `main` assigns to it.
unit unit_init_assign;

extern int puts(const char* s);

class Res {
    ~Res() { puts("res freed"); }
}

Res current;

int main() {
    current = new Res();
    puts("assigned");
    return 0;
}
