// qualified_public_overload — a qualified call sees only public overloads when ranking (#193).
unit qualified_public_overload;

use privovl.lib;

extern int puts(const char* s);

int main() {
    long r = privovl.lib.rankv(5);
    if r != 7 { puts("public overload"); return 1; }
    puts("qualified public overload ok");
    return 0;
}
