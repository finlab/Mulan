// definite_init_terminating_arm — a terminating else arm does not contribute to the initialized set
// (#208).
unit definite_init_terminating_arm;

extern int puts(const char* s);

int f(bool c) {
    int x;
    if c { x = 1; } else { return 0; }
    return x;
}

int main() {
    if f(true) != 1 { puts("value"); return 1; }
    puts("definite init terminating arm ok");
    return 0;
}
