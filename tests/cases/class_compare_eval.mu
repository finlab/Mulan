unit class_compare_eval;
extern int puts(const char* s);
int calls = 0;
int live = 0;
class C { int v; C(int x){v=x;live+=1;} ~C(){live-=1;} int get(){return v;} }
int tick() { calls += 1; return 5; }
C make(int x) { calls += 1; return new C(x); }
C echo(C x) { calls += 1; return x; }
int main() {
    int n = 0;
    if tick() > 4 { n = 1; }
    if calls != 1 { puts("int compare evaluated twice"); return 1; }

    C a = new C(1);
    if echo(a) != a { puts("ref compare wrong"); return 1; }
    if calls != 2 { puts("ref compare evaluated twice"); return 1; }
    if live != 1 { puts("ref compare temp leaked"); return 1; }

    if make(2) == make(2) { puts("identity compare should differ"); return 1; }
    if calls != 4 { puts("double make calls wrong"); return 1; }
    if live != 1 { puts("double make leaked"); return 1; }

    int p = 0;
    int[2] arr = {1, 2};
    if arr[tick() - 5] == 1 { p = 1; }
    if calls != 5 { puts("index eval wrong"); return 1; }
    if p != 1 { puts("index value wrong"); return 1; }

    del a;
    if live != 0 { puts("compare cleanup fail"); return 1; }
    puts("compare eval ok");
    return 0;
}

