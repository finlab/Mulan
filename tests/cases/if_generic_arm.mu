// if_generic_arm.mu — a generic template declared inside a selected `#if` arm is extracted at that
// position (and a template in a dead arm never exists).
unit if_generic_arm;

extern int puts(const char* s);

const bool ON = true;

#if ON {
struct G<T> {
    T v;
}
}
#else {
#error "the template arm was not selected"
}

int main() {
    G<int> g;
    g.v = 7;
    if g.v != 7 { puts("generic arm failed"); return 1; }
    puts("generic arm ok");
    return 0;
}
