// unit_init_catch.mu — an initializer may handle its own exception; nothing aborts
unit unit_init_catch;

extern int puts(const char* s);

class Err : Exception { }

int g = 0;

int f() {
    try {
        throw new Err();
    } catch (e: Err) {
        g = 7;
    }
    return g;
}

int h = f();

int main() {
    if h != 7 { puts("init catch failed"); return 1; }
    puts("caught during init");
    return 0;
}
