// overload_char — a character literal prefers f(char) over f(int) (#160).
unit overload_char;

extern int puts(const char* s);

char pick(char c) { return c; }
int pick(int c) { return c; }

int main() {
    char r = pick('a');
    if r != 'a' { puts("char overload"); return 1; }
    puts("overload char ok");
    return 0;
}
