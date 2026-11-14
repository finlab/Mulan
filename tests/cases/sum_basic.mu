// sum_basic — sum type declaration, construction and value semantics (SPEC 10.1).
// This build has the type, the tag+payload layout, construction and destruction; `match` is next.
unit sum_basic;

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

type Holder = Num(int) | Obj(Box);

int main() {
    // layout: tag + the union payload (an 8-byte payload gives 16 bytes overall)
    if (sizeof(Value) as int) != 16 { printf("sizeof\n"); return 1; }
    if (sizeof(Holder) as int) != 16 { printf("sizeof holder\n"); return 2; }

    // construction of every shape: one field, two fields, a pointer, a unit alternative
    Value a = Value.Int(3);
    Value b = Value.Pair(4, 5);
    Value c = Value.Text("hi");
    Value d = Value.None;
    // copies are byte copies (plus RC for reference payloads)
    Value e = a;
    if sizeof(e) as int != 16 { printf("copy\n"); return 3; }

    // a reference-counted payload is retained by the value and released with it
    {
        Box box = new Box(7);
        Holder h = Holder.Obj(box);
        box = null;
        if live != 1 { printf("retain\n"); return 4; }
        Holder h2 = h;          // copy: the payload is retained again
        if live != 1 { printf("copy retain\n"); return 5; }
        h2 = Holder.Num(0);     // overwrite: the old payload is released
        if live != 1 { printf("overwrite release\n"); return 6; }
    }
    if live != 0 { printf("scope release\n"); return 7; }

    printf("sum basic ok\n");
    return 0;
}
