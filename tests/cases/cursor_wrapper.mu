// cursor_wrapper — build a cursor from two closures with std.cursor, no struct declaration.
unit cursor_wrapper;

use std.cursor;

extern int puts(const char* s);

class Box { public int n; }

int main() {
    int i = 0;
    auto c = cursor(
        [ref i]() => { if i >= 3 { return false; } i += 1; return true; },
        [ref i]() => i
    );
    int s = 0;
    for x in c { s += x; }
    if s != 6 { puts("ref capture"); return 1; }

    Box b = new Box();
    b.n = 0;
    auto c2 = cursor(
        [b]() => { b.n += 1; return b.n <= 2; },
        [b]() => b.n * 10
    );
    int t = 0;
    for x in c2 { t += x; }
    if t != 30 { puts("class capture"); return 2; }

    int n = 0;
    int j = 0;
    auto c3 = cursor(
        [ref j]() => { if j >= 4 { return false; } j += 2; return true; },
        [ref j]() => j
    );
    for x in c3 {
        n += 1;
        if x != n * 2 { puts("sequence"); return 3; }
    }
    if n != 2 { puts("count"); return 4; }

    puts("cursor wrapper ok");
    return 0;
}
