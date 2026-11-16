unit string_null_ctor;
use std.cstdio;

// C APIs such as `getenv` return null; the implicit `String(const char*)` used to dereference it and
// crash. A null input is the empty string, which is what `cstr()` already returns for the zero value.
extern const char* getenv(const char* name);

int main() {
    const char* p = null;
    string s = p;
    if s.length() != 0 {
        printf("bad length\n");
        return 1;
    }
    if s.cstr() == null {
        printf("bad cstr\n");
        return 2;
    }
    string env = getenv("MULAN_SURELY_NOT_SET_12345");
    if env.length() != 0 {
        printf("bad env length\n");
        return 3;
    }
    printf("null ctor ok\n");
    return 0;
}
