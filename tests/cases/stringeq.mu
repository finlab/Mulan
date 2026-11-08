unit stringeq;
extern int puts(readonly char* s);
int main() {
    string a = "abc";
    string b = "abc";
    string c = "abd";
    string z = 0;
    if a == b { puts("eq ok"); }
    if a != c { puts("ne ok"); }
    if z == "" { puts("zero eq empty ok"); }
    if len(z) == 0 { puts("len ok"); }
    if a == "abc" { puts("literal eq ok"); }
    return 0;
}
