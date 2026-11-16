unit string_sub_clamp;
use std.cstdio;

// `sub()` clamps both ends: a negative start/count is zero, an over-long start/count is the source
// length (the comment promised clamping but only the upper bound was handled).
int main() {
    string s = "abcdef";
    string a = s.sub(-5, 3);
    string b = s.sub(0, -1);
    string c = s.sub(4, 100);
    string d = s.sub(100, 2);
    printf("[%s][%s][%s][%s]\n", a.cstr(), b.cstr(), c.cstr(), d.cstr());
    if a != "abc" {
        return 1;
    }
    if b != "" {
        return 2;
    }
    if c != "ef" {
        return 3;
    }
    if d != "" {
        return 4;
    }
    return 0;
}
