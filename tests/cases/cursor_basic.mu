// cursor_basic — the cursor form of `for-in` (SPEC 6.4.2). The cursor is a struct returned by value;
// `next()`/`current()` follow the contract declared in libs/std/interfaces.mu (std.interfaces.IIterator<T>).

unit cursor_basic;

use std.cstdio;

struct Counter {
    public int i;
    public bool next() {
        i = i + 1;
        return i <= 3;
    }
    public int current() { return i * 10; }
}

struct Source {
    public Counter iterator() {
        Counter c = { i = 0 };
        return c;
    }
}

int main() {
    Source s;
    int sum = 0;
    for v in s {
        sum = sum + v;
    }
    printf("%d\n", sum);
    return 0;
}
