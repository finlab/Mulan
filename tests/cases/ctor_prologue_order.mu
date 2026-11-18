unit audit211_order_effects;
extern int puts(const char* s);
int seq = 0;
int f(int tag) { seq = seq * 10 + tag; return tag; }

const class Cfg {
public:
    const int a = 1;
    const int b;
    const int c;
    Cfg(int x) : c(f(3)), b(f(2)) { }
    int sum() const { return a + b + c; }
}

class Chain {
public:
    int n;
    Chain(int v) : n(v) { seq = seq * 10 + 9; }
    Chain() : Chain(4) { seq = seq * 10 + 8; }
    Chain(int x, int y) : Chain(x + y) { seq = seq * 10 + 7; }
}

int main() {
    Cfg c = new Cfg(0);
    if c.sum() != 6 { puts("default + list"); return 1; }
    if seq != 23 { puts("field order"); return 2; }
    seq = 0;
    Chain ch = new Chain(1, 2);
    if ch.n != 3 { puts("chain value"); return 3; }
    if seq != 97 { puts("chain order"); return 4; }
    puts("order ok");
    return 0;
}

