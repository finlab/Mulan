unit string_methods;
extern int puts(const char* s);

int main() {
    string s = "  hello world  ";
    string t = s.trim();
    if t != "hello world" { puts("trim fail"); return 1; }
    if t.length() != 11 { return 2; }
    if t.find("world") != 6 { return 3; }
    if t.find("xyz") != -1 { return 4; }
    if not t.starts_with("hello") { return 5; }
    if not t.ends_with("world") { return 6; }
    if t.starts_with("world") { return 7; }
    if t.is_empty() { return 8; }

    string e = {};
    if not e.is_empty() { return 9; }
    if e.length() != 0 { return 10; }

    string c = string("abc");
    if c != "abc" { return 11; }
    if c.cstr()[3] != 0 { return 12; }
    if c.at(1) != 'b' { return 13; }
    if c.at(99) != 0 { return 14; }

    puts("string methods ok");
    return 0;
}
