// A throw that escapes a `para` section body aborts the process (SPEC 3.17.4, B2). Only the
// throwing section prints, and it prints before throwing, so the output does not depend on how far
// the other section got before the abort.
unit para_throw_section;
extern int printf(const char* fmt, ...);

class Err : Exception {
    Err() : Exception("section") { }
}

int main() {
    para {
        {
            printf("section throws\n");
            throw new Err();
        }
        {
            // deliberately silent: a concurrent print here would race with the abort
        }
    }
    printf("unreachable\n");
    return 0;
}
