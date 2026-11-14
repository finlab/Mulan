unit foronce;
extern int puts(const char* s);

int calls = 0;

int idx() {
    calls += 1;
    return 0;
}

struct Row { int[3] v; }

int main() {
    Row[2] rows = {};
    rows[0].v[0] = 9;
    int t = 0;
    Row r = rows[idx()];
    for i in 0..sizeof(r.v) / sizeof(r.v[0]) {
        t += r.v[i];
    }
    if calls == 1 and t == 9 { puts("seq evaluated once ok"); }

    // value binding copies: changing v does not affect the original array
    for i in 0..sizeof(rows[0].v) / sizeof(rows[0].v[0]) {
        int v = rows[0].v[i];
        v += 100;
    }
    if rows[0].v[0] == 9 { puts("by value ok"); }
    return 0;
}
