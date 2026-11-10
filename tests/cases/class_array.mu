unit class_array;

extern int puts(const char* s);

class Cell {
    int v;
    Cell(int x) { v = x; }
    ~Cell() { }
    int get() { return v; }
    void set(int x) { v = x; }
}

class Grid {
    Cell[2] cells;
    int n;
    Grid() { n = 0; cells[0] = null; cells[1] = null; }
    ~Grid() { }
}

int operator[](Grid g, int i) { return g.cells[i].get(); }
void operator[]=(Grid g, int i, int x) { g.cells[i].set(x); }

int main() {
    Cell[3] cells = {};
    Cell a = new Cell(1);
    Cell b = new Cell(2);
    cells[0] = a;
    cells[1] = b;

    if cells[0].get() != 1 { puts("array store failed"); return 1; }

    Cell c = cells[0];              // 借用 -> 强引用
    del a;                          // 数组里仍然持有
    if cells[0].get() != 1 { puts("array rc failed"); return 1; }
    del c;

    Grid g = new Grid();
    Cell e = new Cell(2);
    g.cells[0] = b;
    g.cells[1] = e;
    g[0] = 5;
    if g[0] != 5 { puts("operator[] failed"); return 1; }
    if g.cells[1].get() != 2 { puts("operator[]= failed"); return 1; }

    del g;
    del e;
    del b;
    puts("class array ok");
    return 0;
}
