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

int main() {
    Cell[3] cells = {};
    Cell a = new Cell(1);
    Cell b = new Cell(2);
    cells[0] = a;
    cells[1] = b;

    if cells[0].get() != 1 { puts("array store failed"); return 1; }

    Cell c = cells[0];              // borrow -> strong reference
    a = null; // still held by the array
    if cells[0].get() != 1 { puts("array rc failed"); return 1; }
    c = null;

    Grid g = new Grid();
    Cell e = new Cell(2);
    g.cells[0] = b;
    g.cells[1] = e;
    g.cells[0].set(5);
    if g[0] != 5 { puts("operator[] failed"); return 1; }
    g.cells[1].set(2);
    if g.cells[1].get() != 2 { puts("operator[]= failed"); return 1; }

    g = null;
    e = null;
    b = null;
    puts("class array ok");
    return 0;
}
