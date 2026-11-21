// enum_alias_match — same-valued enum members are distinct patterns; the first arm wins at runtime
// (#158).
unit enum_alias_match;

extern int puts(const char* s);

enum Flag { A = 1, B = 1 }

int main() {
    Flag f = Flag.A;
    int hit = 0;
    match f {
        A: { hit = 1; }
        B: { hit = 2; }
    }
    if hit != 1 { puts("alias"); return 1; }
    puts("enum alias match ok");
    return 0;
}
