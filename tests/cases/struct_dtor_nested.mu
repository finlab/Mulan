// struct_dtor_nested — a struct with a user destructor as a struct field, a static array element and a
// class field (agents/value-semantics.md §4). Construction and destruction are recursive; copying such a
// value is still refused while copy construction is unimplemented (tests/errors/struct_dtor_field).
unit struct_dtor_nested;

extern int printf(const char* fmt, ...);

int live = 0;
int ctors = 0;
int dtors = 0;

struct S {
    int x;
    S() {
        x = 7;
        ctors = ctors + 1;
        live = live + 1;
    }
    ~S() {
        dtors = dtors + 1;
        live = live - 1;
    }
}

struct Outer { S s; int y; }

class K { public S s; }

// Explicit `super(...)`: the base constructor call is a statement, and it is the one that runs the member
// step for the derived class - the constructor must not run it a second time (a second construction would
// leak the first member).
class KB { public S bs; KB() { } }
class KD : KB { public S ds; KD() : KB() { } }

int main() {
    {
        Outer o;
        if o.s.x != 7 { printf("field ctor\n"); return 1; }
    }
    if live != 0 { printf("field dtor\n"); return 2; }

    {
        S[3] a;
        if a[0].x != 7 { printf("array ctor 0\n"); return 3; }
        if a[2].x != 7 { printf("array ctor 2\n"); return 4; }
        if live != 3 { printf("array live\n"); return 5; }
    }
    if live != 0 { printf("array dtor\n"); return 6; }

    {
        K k = new K();
        if k.s.x != 7 { printf("class field ctor\n"); return 7; }
        k = null;               // the class destructor releases the struct member
        if live != 0 { printf("class field dtor\n"); return 8; }
    }

    {
        Outer[2] grid;
        if grid[1].s.x != 7 { printf("nested ctor\n"); return 9; }
        if live != 2 { printf("nested live\n"); return 10; }
    }
    if live != 0 { printf("nested dtor\n"); return 11; }

    {
        KD d = new KD();
        if d.bs.x != 7 { printf("super base member\n"); return 12; }
        if d.ds.x != 7 { printf("super derived member\n"); return 13; }
        if live != 2 { printf("super live\n"); return 14; }
        d = null;
        if live != 0 { printf("super dtor\n"); return 15; }
    }
    if ctors != dtors { printf("unbalanced %d %d\n", ctors, dtors); return 16; }

    printf("ctors %d dtors %d\n", ctors, dtors);
    printf("struct_dtor_nested ok\n");
    return 0;
}
