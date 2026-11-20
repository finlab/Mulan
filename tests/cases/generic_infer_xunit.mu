unit generic_infer_xunit;

use genx.infer;
use std.option;

extern int puts(const char* s);

int main() {
    Box<int> a = make();
    if a.v != 0 { puts("target across unit"); return 1; }

    Pair<int, string> p = make_pair<int>();
    if p.a != 0 { puts("partial target across unit"); return 2; }

    int x = first_like(Some(5), 0);
    if x != 5 { puts("deferred arg across unit"); return 3; }

    Option<int> o = id(Some(7));
    if o is None { puts("target arg across unit"); return 4; }
    if unwrap_or(o, -1) != 7 { puts("target arg value"); return 5; }

    puts("generic infer xunit ok");
    return 0;
}
