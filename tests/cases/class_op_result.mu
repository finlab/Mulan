unit class_op_result;
extern int puts(const char* s);
int live = 0;
class P { int v; P(int x){v=x;live+=1;} ~P(){live-=1;} int get(){return v;} }
P operator+(P a, P b) { return new P(a.get() + b.get()); }
int main() {
    P r = new P(1) + new P(2);
    if r.get() != 3 { puts("op new fail"); return 1; }
    if live != 1 { puts("op new leak"); return 1; }
    r = null;
    if live != 0 { puts("op new cleanup fail"); return 1; }
    puts("op new ok");
    return 0;
}

