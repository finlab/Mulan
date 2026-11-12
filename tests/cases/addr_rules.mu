unit addr_rules;

// U21/U24: allowed address-of forms (plain value struct / fixed array element / pointer variable /
// scalar field of a struct with reference fields / struct with a user destructor)
extern int puts(const char* s);

class C { int v; }
struct P { int x; int y; }
struct Outer { P p; int[3] v; }
struct R { C c; int n; }      // reference fields: whole-object & refused, scalar fields ok
struct D { int x; ~D() {} }   // user destructor: still used via pointers, & allowed

int main() {
    P a = {1, 2};
    int* px = &a.x;
    P* pa = &a;
    int[3] arr = {3, 4, 5};
    int* pe = &arr[1];
    int n = 6;
    int* pn = &n;
    int** pp = &pn;
    Outer o = {{7, 8}, {9, 10, 11}};
    int* po = &o.p.y;
    int* pv = &o.v[2];

    R r = {};
    int* pr = &r.n;
    *pr = 5;

    D d = {};
    D* pd = &d;
    (*pd).x = 7;

    if *px != 1 or (*pa).x != 1 or *pe != 4 or **pp != 6 or *po != 8 or *pv != 11 {
        puts("addr rules failed");
        return 1;
    }
    if r.n != 5 or d.x != 7 {
        puts("addr rules field failed");
        return 2;
    }
    puts("addr rules ok");
    return 0;
}
