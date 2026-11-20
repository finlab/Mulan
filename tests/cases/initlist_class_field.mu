// initlist_class_field.mu — a class-handle field set by the constructor initializer list is retained like
// any other assignment (it used to be stored raw, so the parameter's release freed the object), and a
// struct's handle field can be passed on and read.
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

struct OtherHolder {
    public Cell2 c;
    OtherHolder(Cell2 x) : c(x) { }
}

int readIt(const Cell2 c) { return c.v; }

int main() {
    Holder h = Holder(new Cell2(1));
    Cell2 local = h.c;
    if local.v != 1 { puts("init-list class field failed"); return 1; }

    OtherHolder ch = OtherHolder(new Cell2(2));
    if readIt(ch.c) != 2 { puts("struct handle field failed"); return 2; }
    if ch.c.v != 2 { puts("struct handle field read failed"); return 3; }
    puts("initlist class field ok");
    return 0;
}

