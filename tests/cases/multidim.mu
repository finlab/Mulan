unit multidim;
extern int puts(const char* s);
int live = 0;

class C {
    int v;
    C(int x) { v = x; live += 1; }
    ~C() { live -= 1; }
    int get() { return v; }
}

struct S { int[] b; }

int main() {
    // 静态矩形 T[m,n]
    int[2,3] a = {{1, 2, 3}, {4, 5, 6}};
    if len(a) != 2 { puts("static dim0"); return 1; }
    if len(a, 1) != 3 { puts("static dim1"); return 2; }
    if a[0,1] != 2 { puts("static idx 0,1"); return 3; }
    if a[1,2] != 6 { puts("static idx 1,2"); return 4; }
    if a[1][0] != 4 { puts("static idx [1][0]"); return 5; }
    a[1,2] = 9;
    if a[1][2] != 9 { puts("static write"); return 6; }
    int[3] row = a[0];
    if row[2] != 3 { puts("static row"); return 7; }
    if sizeof(int[2,3]) != 24 { puts("static sizeof"); return 8; }

    // 三维静态
    int[2,2,2] c3 = {};
    c3[1,0,1] = 9;
    if c3[1,0,1] != 9 { puts("static 3d"); return 9; }
    if len(c3, 2) != 2 { puts("static 3d dim"); return 10; }

    // 动态矩形 T[,]
    int m = 2;
    int n = 3;
    int[,] b = new int[m,n];
    if len(b) != 2 { puts("dyn dim0"); return 11; }
    if len(b, 1) != 3 { puts("dyn dim1"); return 12; }
    if b[1,2] != 0 { puts("dyn zero"); return 13; }
    int i = 1;
    int j = 2;
    b[i,j] = 7;
    if b[1,2] != 7 { puts("dyn write"); return 14; }
    int k = 1;
    if len(b, k) != 3 { puts("dyn runtime dim"); return 15; }
    int[,] b2 = b;
    b2[0,0] = 5;
    if b[0,0] != 5 { puts("dyn share"); return 16; }
    int sum = 0;
    for x in b { sum += x; }
    if sum != 12 { puts("dyn for"); return 17; }

    // 动态三维
    int[,,] d3 = new int[2,2,2];
    d3[1,1,1] = 7;
    if d3[1,1,1] != 7 { puts("dyn 3d"); return 18; }
    if len(d3, 2) != 2 { puts("dyn 3d dim"); return 19; }

    // 静态矩形里的 class 引用：逐元素 RC
    {
        C[2,2] cs = {};
        cs[0,1] = new C(1);
        cs[1,0] = new C(2);
        if live != 2 { puts("static ref elems"); return 20; }
        C[2,2] cs2 = cs;
        if live != 2 { puts("static ref copy"); return 21; }
        cs2[0,1] = new C(3);
        if live != 3 { puts("static ref assign"); return 22; }
        if cs[0,1].get() != 1 { puts("static ref old"); return 23; }
        if cs2[1,0].get() != 2 { puts("static ref shared"); return 24; }
    }
    if live != 0 { puts("static ref cleanup"); return 25; }

    // 动态矩形里的 class 引用
    {
        C[,] cs = new C[2,2];
        cs[0,1] = new C(4);
        if live != 1 { puts("dyn ref elem"); return 26; }
        C[,] cs2 = cs;
        cs2[1,0] = new C(5);
        if live != 2 { puts("dyn ref assign"); return 27; }
        if cs[1,0].get() != 5 { puts("dyn ref shared"); return 28; }
    }
    if live != 0 { puts("dyn ref cleanup"); return 29; }

    // 静态矩形里的 struct（含动态数组字段）
    {
        S[2,2] ss = {};
        ss[0,0].b = new int[2];
        ss[0,0].b[1] = 5;
        if ss[0,0].b[1] != 5 { puts("rect struct field"); return 30; }
        S[2,2] st = ss;
        if st[0,0].b[1] != 5 { puts("rect struct copy"); return 31; }
    }

    // 锯齿数组：外层 new 一次一维，循环填行
    int[][] jg = new int[3][];
    if len(jg) != 3 { puts("jagged outer"); return 32; }
    if len(jg[0]) != 0 { puts("jagged null row"); return 33; }
    for r in 0..3 {
        jg[r] = new int[r + 1];
        jg[r][0] = r;
    }
    if len(jg[2]) != 3 { puts("jagged row len"); return 34; }
    if jg[2][0] != 2 { puts("jagged row value"); return 35; }
    if jg[2][2] != 0 { puts("jagged row zero"); return 36; }

    // 定长行槽的锯齿：int[][N]
    int[][2] js = {};
    js[0] = new int[2];
    js[1] = new int[3];
    js[1][2] = 8;
    if js[1][2] != 8 { puts("fixed jagged"); return 37; }
    if len(js[0]) != 2 { puts("fixed jagged len"); return 38; }

    // 静态矩形 -> 动态矩形：深复制元素
    int[,] conv = a;
    if conv[1,2] != 9 { puts("rect convert value"); return 39; }
    conv[1,2] = 1;
    if a[1,2] != 9 { puts("rect convert deep"); return 40; }

    // 定长行槽 -> 锯齿：复制外层并 retain 行（行共享）
    int[][] jconv = js;
    if len(jconv[1]) != 3 { puts("jagged convert len"); return 41; }
    jconv[1][2] = 8;
    if js[1][2] != 8 { puts("jagged convert share"); return 42; }

    puts("multidim ok");
    return 0;
}
