unit use_list;

// One `use` imports several units: `use a, b.c, d;`. A trailing comma is accepted. The statement is
// atomic — a missing unit imports nothing (the loader reports it before any of the names are bound).

use std.cstdio, std.string,;

int main() {
    string s = string("ok");
    printf("%s %d\n", s.cstr(), s.length() as int);
    return 0;
}
