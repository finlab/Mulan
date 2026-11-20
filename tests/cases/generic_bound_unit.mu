// generic_bound_unit — a bound is resolved in the template's unit: `use std.unicode` alone must be
// enough for its public generic API (the caller does not import std.utf8, where IString is declared).
unit generic_bound_unit;

use std.string;
use std.unicode;

extern int puts(const char* s);

int main() {
    string h = "hello";
    string t = unicode_copy_range(h, 0, 5);
    if t.length() != 5 { puts("length"); return 1; }
    puts("generic bound unit ok");
    return 0;
}
