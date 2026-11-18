// initlist_class_field.mu — a class-handle field set by the constructor initializer list is retained like
// any other assignment (it used to be stored raw, so the parameter's release freed the object), and a
// `const struct`'s handle field can be passed on and read.
unit initlist_class_field;

extern int puts(const char* s);

class Cell2 {
    public int v;
    Cell2(int x) : v(x) { }
}

struct Holder {
    public Cell2 c;
    Holder(Cell2 x) : c(x) { }
}

const struct ConstHolder {
    public Cell2 c;
    ConstHolder(Cell2 x) : c(x) { }
}

int readIt(const Cell2 c) { return c.v; }

int main() {
    Holder h = Holder(new Cell2(1));
    Cell2 local = h.c;
    if local.v != 1 { puts("init-list class field failed"); return 1; }

    const ConstHolder ch = ConstHolder(new Cell2(2));
    if readIt(ch.c) != 2 { puts("const struct handle field failed"); return 2; }
    if ch.c.v != 2 { puts("const struct handle field read failed"); return 3; }
    puts("initlist class field ok");
    return 0;
}

