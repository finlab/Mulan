// string_compare.mu — string equality compares the bytes, not the identity
//
// `strings.mu` covers length/index/for-in only, so the comparison operators of the
// library string type were untested.
unit string_compare;

use std.cstdio;
use std.string;

// The stdlib provides no ordering operator for `string` (StdLibSPEC 6.3 lists `==`
// only), so `a < b` on two strings is a compile error. An operator function may live
// in any unit (SPEC 3.11), so a program can supply the ordering itself; the
// comparison below is lexicographic over the bytes.
bool operator<(String a, String b) {
    isize n = a.length();
    if b.length() < n { n = b.length(); }
    isize i = 0;
    while i < n {
        if a[i] != b[i] { return a[i] < b[i]; }
        i += 1;
    }
    return a.length() < b.length();
}

int main() {
    string a = "hello";
    string b = "hello";       // a different buffer with the same bytes
    string c = "hellx";       // same length, different last byte
    string d = "hell";        // a prefix of a
    string e = {};            // the empty string
    string f = "Hello";       // case matters
    string g = string("hello");

    printf("eq=%d\n", a == b);             // 1: equal bytes
    printf("eq-copy=%d\n", a == g);        // 1: an explicit copy is equal too
    printf("ne=%d\n", a != c);             // 1
    printf("neq=%d\n", a == c);            // 0
    printf("lit=%d\n", a == "hello");      // 1: a literal compares by bytes
    printf("lit-left=%d\n", "hello" == a);// 1: the literal may be on the left
    printf("ne-lit=%d\n", a != "hellx");   // 1
    printf("empty=%d\n", e == "");         // 1: the empty string
    printf("prefix=%d\n", a != d);         // 1: a different length is not equal
    printf("case=%d\n", a != f);           // 1: the comparison is byte exact

    // ---- ordering (supplied by this unit, see above) ----
    string x = "x";
    string y = "y";
    printf("lt=%d\n", x < y);              // 1
    printf("lt-eq=%d\n", a < b);           // 0: equal strings are not less
    printf("lt-prefix=%d\n", d < a);       // 1: a prefix is less
    printf("lt-case=%d\n", f < a);         // 1: 'H' (0x48) is less than 'h' (0x68)

    puts("string compare ok");
    return 0;
}
