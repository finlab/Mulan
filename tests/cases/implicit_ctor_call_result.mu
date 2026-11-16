unit implicit_ctor_call_result;
use std.cstdio;
use std.string;

// A call result of the source type goes through the same implicit constructor as a variable:
// `string c = s.cstr();` is `string b = p;` with `p` written inline (SPEC 3.6 coercion points).
string make() {
    return "made";
}

int main() {
    string s = "hello";
    string a = s.cstr();
    string b = make();
    const char* p = "lit";
    string c = p;
    printf("[%s][%s][%s]\n", a.cstr(), b.cstr(), c.cstr());
    if a != "hello" {
        return 1;
    }
    if b != "made" {
        return 2;
    }
    if c != "lit" {
        return 3;
    }
    return 0;
}
