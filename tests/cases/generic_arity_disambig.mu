// generic_arity_disambig — an explicit type-argument list disambiguates same-name templates by arity
// before the ambiguity check (#249).
unit generic_arity_disambig;

use std.result;

extern int puts(const char* s);

int main() {
    Result<int, int> e = Err(7);
    if unwrap_or<int, int>(e, 0) != 0 { puts("unwrap"); return 1; }
    puts("generic arity disambig ok");
    return 0;
}
