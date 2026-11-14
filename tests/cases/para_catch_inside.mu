// An exception caught inside a section is ordinary control flow: the region joins normally
// (SPEC 3.17.4, last clause). A section cannot write an enclosing local (3.17.2), so the catch
// prints instead of counting.
unit para_catch_inside;
extern int printf(const char* fmt, ...);

class Err : Exception {
    Err() { super("inner"); }
}

int main() {
    para {
        {
            try {
                throw new Err();
            } catch (e: Err) {
                printf("caught inside\n");
            }
        }
        {
        }
    }
    printf("after region\n");
    return 0;
}
