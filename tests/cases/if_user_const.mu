// if_user_const.mu — `#if` conditions may read unit-level compile-time constants declared before them
unit if_user_const;

extern int puts(const char* s);

const bool DEBUG = true;
const int LEVEL = 2;

#if DEBUG {
    const int A = 1;
}
#if LEVEL == 2 {
    const int B = 2;
}
#if LEVEL == 3 {
    const int C = 3;   // this arm is not selected
}

int main() {
    if A != 1 { puts("bool condition failed"); return 1; }
    if B != 2 { puts("int condition failed"); return 2; }
    puts("if user const ok");
    return 0;
}
