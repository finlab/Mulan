unit ternary;

extern int puts(const char* s);

int live = 0;
int calls = 0;
int side = 0;

class C {
    int v;
    C(int n) { v = n; live += 1; }
    ~C() { live -= 1; }
}

struct P { int x; int y; }

enum Color { Red, Green, Blue }

int tag(int n) {
    calls += 1;
    return n;
}

void side_a() { side += 1; }
void side_b() { side += 100; }

int main() {
    bool pick = true;
    int a = 1;

    // basic form
    int x = a > 0 ? 10 : 20;
    if x != 10 { puts("then fail"); return 1; }
    x = a < 0 ? 10 : 20;
    if x != 20 { puts("else fail"); return 2; }

    // only the selected branch is evaluated
    int v = a > 0 ? tag(10) : tag(20);
    if v != 10 or calls != 1 { puts("short-circuit fail"); return 3; }

    // right-associative: a ? b : c ? d : e == a ? b : (c ? d : e)
    int z = 0;
    int r = z == 0 ? 1 : z == 1 ? 2 : 3;
    if r != 1 { puts("nested then fail"); return 4; }
    r = z == 5 ? 1 : z == 1 ? 2 : 3;
    if r != 3 { puts("nested else fail"); return 5; }

    // literal adapts to the context / implicit widening
    double d = a > 0 ? 1 : 2.5;
    if d != 1.0 { puts("literal adapt fail"); return 6; }
    float f = 1.5;
    double d2 = a > 0 ? f : 2.0;
    if d2 != 1.5 { puts("widen fail"); return 7; }

    // struct: value copy
    P p1 = {1, 2};
    P p2 = {3, 4};
    P p3 = pick ? p1 : p2;
    if p3.x != 1 or p3.y != 2 { puts("struct fail"); return 8; }

    // null adapts to the other side
    int* np = pick ? null : &a;
    if np != null { puts("null fail"); return 9; }

    // enum: only mixes with the same enum type
    Color col = pick ? Red : Blue;
    if col != Red { puts("enum fail"); return 10; }

    // void branch: usable only as an expression statement
    pick ? side_a() : side_b();
    if side != 1 { puts("void fail"); return 11; }

    // class reference: the borrowed branch retains, the owned branch transfers; only the selected object is constructed
    {
        C c1 = new C(1);
        C c2 = new C(2);
        if live != 2 { puts("live a fail"); return 12; }
        C c3 = pick ? c1 : c2;
        if live != 2 { puts("live b fail"); return 13; }
        if c3.v != 1 { puts("ref fail"); return 14; }

        pick = false;
        C c4 = pick ? new C(7) : new C(8);
        if live != 3 { puts("live c fail"); return 15; }
        if c4.v != 8 { puts("new fail"); return 16; }
        pick = true;
    }
    if live != 0 { puts("rc cleanup fail"); return 17; }

    // string (owner RC): borrowed and owned branches
    string s1 = "abc";
    string s2 = "de";
    string s3 = pick ? s1 : s2;
    if s3.length() != 3 { puts("string fail"); return 18; }
    string s4 = pick ? s1.concat(s2) : s2;
    if s4.length() != 5 { puts("string owned fail"); return 19; }

    puts("ternary ok");
    return 0;
}
