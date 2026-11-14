// sum_switch — `match` on a sum type (SPEC 10.1.1): bindings, exhaustiveness, `else`, and reference
// payloads (the bound local owns a reference for the duration of the arm).
unit sum_match;

use std.cstdio;

type Value = Int(int) | Pair(int, int) | Text(const char*) | None;

int live = 0;

class Box {
    public int id;
    Box(int x) {
        id = x;
        live = live + 1;
    }
    ~Box() { live = live - 1; }
}

type Holder = Obj(Box) | Empty;

// a sum-typed parameter: dispatch is the only way to read the payload
int describe(Value v) {
    switch v {
        Int(n): { return n * 2; }
        Pair(a, b): { return a * 10 + b; }
        Text(s): { return 100; }
        None: { return -1; }
    }
}

int main() {
    if describe(Value.Int(21)) != 42 { printf("int\n"); return 1; }
    if describe(Value.Pair(4, 5)) != 45 { printf("pair\n"); return 2; }
    if describe(Value.Text("hi")) != 100 { printf("text\n"); return 3; }
    if describe(Value.None) != -1 { printf("none\n"); return 4; }

    // `else` covers the rest
    Value v = Value.Pair(1, 2);
    int seen = 0;
    switch v {
        Int(n): { seen = 1; }
        else: { seen = 9; }
    }
    if seen != 9 { printf("else\n"); return 5; }

    // a reference payload: the binding keeps the object alive inside the arm and releases it after
    Holder h = Holder.Obj(new Box(7));
    int id = 0;
    switch h {
        Obj(b): {
            id = b.id;
            if live != 1 { printf("binding live\n"); return 6; }
        }
        Empty: { id = -1; }
    }
    if id != 7 { printf("payload\n"); return 7; }
    if live != 1 { printf("after arm\n"); return 8; }

    // the sum itself releases its payload at scope exit
    {
        Holder tmp = Holder.Obj(new Box(8));
        int t = 0;
        switch tmp { Obj(b): { t = b.id; } Empty: { t = 0; } }
        if t != 8 { printf("temp\n"); return 9; }
    }
    if live != 1 { printf("temp released\n"); return 10; }

    printf("sum switch ok\n");
    return 0;
}
