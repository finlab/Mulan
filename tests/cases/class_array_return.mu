unit class_array_return;
extern int puts(const char* s);
int live = 0;
class C { int v; C(int x){v=x;live+=1;} ~C(){live-=1;} int get(){return v;} }
C[2] make() {
    C[2] t = {};
    t[0] = new C(5);
    t[1] = new C(6);
    return t;
}
int total(C[2] arr) { return arr[0].get() + arr[1].get(); }
int main() {
    C[2] r = make();
    if r[0].get() != 5 or r[1].get() != 6 { puts("array return fail"); return 1; }
    if live != 2 { puts("array return rc fail"); return 1; }
    int s = total(make());              // 临时实参，调用后释放
    if s != 11 { puts("array temp arg fail"); return 1; }
    if live != 2 { puts("array temp cleanup fail"); return 1; }
    puts("array return ok");
    return 0;
}

