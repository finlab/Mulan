// sum_generic — generic sum types (SPEC 10.1.1, stage 3): `type Option<T> = Some(T) | None;` and
// `type Result<T, E> = Ok(T) | Err(E);` instantiate per type argument, like a generic struct.
unit sum_generic;

use std.cstdio;

type Option<T> = Some(T) | None;
type Result<T, E> = Ok(T) | Err(E);

int live = 0;

class Box {
    public int id;
    Box(int x) {
        id = x;
        live = live + 1;
    }
    ~Box() { live = live - 1; }
}

int unwrap_or(Option<int> o, int fallback) {
    match o {
        Some(v): { return v; }
        None: { return fallback; }
    }
}

int main() {
    // one instance per type argument; each has its own alternatives
    Option<int> a = Option<int>.Some(3);
    Option<int> b = Option<int>.None;
    if unwrap_or(a, -1) != 3 { printf("some\n"); return 1; }
    if unwrap_or(b, -1) != -1 { printf("none\n"); return 2; }

    Option<const char*> s = Option<const char*>.Some("hi");
    int n = 0;
    match s {
        Some(p): { n = 1; }
        None: { n = 0; }
    }
    if n != 1 { printf("pointer payload\n"); return 3; }

    // two parameters, and a payload that needs reference counting
    Result<int, const char*> r = Result<int, const char*>.Ok(5);
    int v = 0;
    match r {
        Ok(x): { v = x; }
        Err(e): { v = -1; }
    }
    if v != 5 { printf("result\n"); return 4; }

    {
        Option<Box> ob = Option<Box>.Some(new Box(9));
        int id = 0;
        match ob {
            Some(x): { id = x.id; }
            None: { id = -1; }
        }
        if id != 9 { printf("rc payload\n"); return 5; }
    }
    if live != 0 { printf("rc release\n"); return 6; }

    // different instantiations are different types: sizeof is independent per payload
    if (sizeof(Option<int>) as int) != 8 { printf("sizeof int\n"); return 7; }
    if (sizeof(Option<const char*>) as int) != 16 { printf("sizeof ptr\n"); return 8; }

    printf("sum generic ok\n");
    return 0;
}
