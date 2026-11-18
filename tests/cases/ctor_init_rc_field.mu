// ctor_init_rc_field — a reference-counted field written through the constructor initializer list
// keeps the same ownership rules as a field assignment: a borrowed source retains, an owned one
// transfers. A delegate field left unretained was released once too often at scope exit.
unit ctor_init_rc_field;

use std.cstdio;

type Get = delegate () => int;

class Cell {
    public int n;
    Cell(int x) { n = x; }
    ~Cell() { }
}

struct Holder {
    public Get g;
    Holder(Get x) : g(x) { }
}

struct CellHolder {
    public Cell c;
    CellHolder(Cell x) : c(x) { }
}

Holder build() {
    int v = 21;
    Get g = [v] () => v;
    return Holder(g);
}

int main() {
    // a borrowed delegate: the environment outlives the source variable only if the write retains it
    {
        int v = 7;
        Get g = [v] () => v;
        Holder h = Holder(g);
        if h.g() != 7 { printf("delegate\n"); return 1; }
    }

    // the source is gone before the field is used
    Holder h2 = build();
    if h2.g() != 21 { printf("delegate outlived\n"); return 2; }

    // an owned source (a fresh lambda) transfers: no extra retain, no leak
    int v3 = 42;
    Holder h3 = Holder([v3] () => v3 + 1);
    if h3.g() != 43 { printf("owned delegate\n"); return 3; }

    // a class handle field through the initializer list, with the source released first
    CellHolder ch = CellHolder(new Cell(9));
    if ch.c.n != 9 { printf("class field\n"); return 4; }

    printf("ctor init rc ok\n");
    return 0;
}
