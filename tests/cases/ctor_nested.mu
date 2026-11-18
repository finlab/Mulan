// ctor_nested — implicit construction of members and array elements (agents/value-semantics.md §5):
// a member/array element of a type with a default constructor is constructed in place, in C++ order
// (base constructor, then members in declaration order, then the body).
unit ctor_nested;

extern int printf(const char* fmt, ...);

int ctors = 0;

struct S {
    int x;
    S() {
        x = 5;
        ctors = ctors + 1;
    }
}

struct O { S s; int y; }
struct Twice { S a; S b; }

class Base {
    public S bs;
    Base() { printf("base %d\n", bs.x); }
}

class Derived : Base {
    public S ds;
    Derived() : Base() {
        printf("derived %d %d\n", bs.x, ds.x);
    }
}

class NoCtor { public S s; }

int main() {
    Twice t;
    if t.a.x != 5 { printf("field\n"); return 1; }
    if t.b.x != 5 { printf("field2\n"); return 2; }

    S[2,3] grid;
    if grid[0,0].x != 5 { printf("static array\n"); return 3; }
    if grid[1,2].x != 5 { printf("static array 2\n"); return 4; }

    O o = { S(), 7 };
    if o.s.x != 5 { printf("aggregate\n"); return 5; }

    O z = {};                 // value-initialization: zero, then the member constructors
    if z.s.x != 5 { printf("empty braces\n"); return 6; }

    Derived d = new Derived(); // members between the base constructor and the body
    if d.ds.x != 5 { printf("class member\n"); return 7; }

    NoCtor nc = new NoCtor();  // a class without a constructor of its own
    if nc.s.x != 5 { printf("class without ctor\n"); return 8; }

    S[4] part = { S() };       // the elements the initializer omits are still constructed
    if part[0].x != 5 { printf("partial 0\n"); return 9; }
    if part[3].x != 5 { printf("partial 3\n"); return 10; }

    printf("constructors run: %d\n", ctors);
    printf("ctor_nested ok\n");
    return 0;
}
