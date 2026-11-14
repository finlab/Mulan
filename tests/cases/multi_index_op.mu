// multi_index_op.mu — multi-index `operator[]` on user-defined types (member and free forms)
unit multi_index_op;

extern int puts(const char* s);

struct Grid {
    int[2,3] values;
    ref int operator[](usize i, usize j) { return values[i,j]; }
}

struct Table {
    int[2,2] v;
}

ref int operator[](ref Table t, usize i, usize j) { return t.v[i,j]; }

int main() {
    Grid g;
    g[1,2] = 7;
    if g[1,2] != 7 { puts("member multi-index failed"); return 1; }

    Table t;
    t[0,1] = 9;
    if t[0,1] != 9 { puts("free multi-index failed"); return 1; }

    puts("multi index ok");
    return 0;
}
