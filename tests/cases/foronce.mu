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
    for v in rows[idx()].v {
        t += v;
    }
    if calls == 1 and t == 9 { puts("seq evaluated once ok"); }

    // 值绑定拷贝：改 v 不影响原数组
    for v in rows[0].v {
        v += 100;
    }
    if rows[0].v[0] == 9 { puts("by value ok"); }
    return 0;
}
