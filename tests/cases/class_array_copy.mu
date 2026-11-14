unit class_array_copy;
extern int puts(const char* s);
int live = 0;
class C { int v; C(int x){v=x;live+=1;} ~C(){live-=1;} int get(){return v;} }
int main() {
    C[2] a = {};
    a[0] = new C(1);
    a[1] = new C(2);
    C[2] b = {};
    for i in 0..2 { b[i] = a[i]; }   // element-wise: arrays themselves are not assignable (C99)
    a[0] = null;
    if b[0].get() != 1 { puts("array copy value fail"); return 1; }
    b[0] = null;
    if live != 1 { puts("array rc fail"); return 1; }
    puts("array copy ok");
    return 0;
}

