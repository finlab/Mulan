unit dynarray;
extern int puts(const char* s);
int live = 0;

class C {
    int v;
    C(int x) { v = x; live += 1; }
    ~C() { live -= 1; }
    int get() { return v; }
}

struct S { byte[] b; }
struct Box2 { int[] b; }

int addlen(byte[] b, int x) { return (len(b) as int) + x; }
S mk(int n) {
    S s = {new byte[n]};
    return s;
}
void keep(Box2* out, int[] b) { (*out).b = b; }

const int[] G = {10, 20, 30};

int main() {
    // new / len / 下标
    byte[] b = new byte[4];
    b[0] = 7;
    b[1] = 9;
    if len(b) != 4 { puts("len failed"); return 1; }
    if b[0] != 7 or b[1] != 9 { puts("index failed"); return 2; }
    if len(new byte[5]) != 5 { puts("temp len failed"); return 3; }
    if (new byte[2])[0] != 0 { puts("temp index failed"); return 4; }

    // 零值
    int[] z = {};
    if len(z) != 0 { puts("zero slice failed"); return 5; }
    int n = 0;
    for v in z { n += 1; }
    if n != 0 { puts("empty for failed"); return 6; }

    // 聚合初始化
    int[] q = {1, 2, 3};
    if len(q) != 3 or q[2] != 3 { puts("aggregate failed"); return 7; }

    // 定长数组 -> 动态数组：复制，不共享
    int[3] a = {1, 2, 3};
    int[] r = a;
    r[0] = 8;
    if a[0] != 1 { puts("array must not alias"); return 8; }
    if r[0] != 8 { puts("array copy failed"); return 9; }

    // 动态数组：句柄共享
    int[] t = q;
    t[0] = 9;
    if q[0] != 9 { puts("handle share failed"); return 10; }
    int[] self = {4, 5};
    self = self;
    if self[1] != 5 { puts("self assign failed"); return 11; }
    if addlen(b, 1) != 5 { puts("param failed"); return 12; }
    if G[2] != 30 or len(G) != 3 { puts("global failed"); return 13; }

    // 元素为 class 引用：数组负责元素生命周期
    {
        C[] cs = new C[2];
        cs[0] = new C(1);
        if live != 1 { puts("elem retain failed"); return 14; }
        {
            C[] cs2 = cs;
            cs2[1] = new C(2);
            if live != 2 { puts("elem assign failed"); return 15; }
            if cs[1].get() != 2 { puts("elem shared failed"); return 16; }
        }
        if live != 2 { puts("array release failed"); return 17; }
        int cnt = 0;
        for c in cs { cnt += c.get(); }
        if cnt != 3 { puts("elem for failed"); return 18; }
    }
    if live != 0 { puts("elem destroy failed"); return 19; }

    // 赋值替换旧数组 -> 旧数组归零销毁
    {
        C[] old = new C[1];
        old[0] = new C(7);
        if live != 1 { puts("assign elem setup failed"); return 20; }
        old = new C[1];
        if live != 0 { puts("assign release old failed"); return 21; }
    }

    // struct 字段 / 拷贝 / 返回
    {
        S st = mk(3);
        if len(st.b) != 3 { puts("struct field failed"); return 22; }
        S st2 = st;
        st2.b[0] = 4;
        if st.b[0] != 4 { puts("struct share failed"); return 23; }
    }

    // 借用参数存进字段 -> retain；实参临时数组在调用后释放
    {
        Box2 box = {};
        keep(&box, a);
        if len(box.b) != 3 or box.b[1] != 2 { puts("borrow store failed"); return 24; }
        box.b[0] = 33;
        if a[0] != 1 { puts("stored copy failed"); return 25; }
    }

    // for ref 修改共享数组
    int sum = 0;
    for v in q { sum += v; }
    if sum != 14 { puts("for failed"); return 26; }
    for ref v in q { v += 1; }
    if q[0] != 10 { puts("for ref failed"); return 27; }

    puts("dynamic array ok");
    return 0;
}
