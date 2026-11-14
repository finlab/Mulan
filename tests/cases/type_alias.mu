// type_alias.mu — type alias `type A = B;` (transparent alias, no new type)
//
// Covers: scalar/pointer/slice/fixed array/struct aliases, alias of alias, aliases as fields and array elements,
// `sizeof`/`len`/`const` combined with aliases.
unit type_alias;

extern int puts(const char* s);

struct Point {
    int x, y;
}

type int32 = int;
type IntPtr = int*;
type IntArr = int[];
type P = Point;
type PP = P;
type Vec3 = int[3];

struct Box {
    P p;
    int32 n;
    Vec3 v;
}

int main() {
    Box b = {{1, 2}, 3, {4, 5, 6}};
    if b.p.x == 1 and b.p.y == 2 and b.n == 3 and b.v[2] == 6 {
        puts("struct ok");
    }

    PP q = {7, 8};
    if q.x == 7 and q.y == 8 {
        puts("alias-of-alias ok");
    }

    IntArr xs = new int[3];
    xs[0] = 10;
    IntPtr p = &xs[0];
    if *p == 10 {
        puts("ptr ok");
    }

    if sizeof(P) == sizeof(Point) {
        puts("sizeof ok");
    }
    if sizeof(int32) == 4 {
        puts("width ok");
    }
    if xs.length() == 3 {
        puts("slice ok");
    }

    const int32 k = 5;
    if k == 5 {
        puts("const ok");
    }

    P[] arr = new P[2];
    arr[0] = q;
    if arr[0].y == 8 {
        puts("array elem ok");
    }

    Vec3 v3 = {9, 8, 7};
    if v3[1] == 8 {
        puts("fixed array ok");
    }
    return 0;
}
