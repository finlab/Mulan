// type_alias.mu — 类型别名 `type A = B;`（透明别名，不产生新类型）
//
// 覆盖：标量/指针/切片/定长数组/struct 别名、别名套别名、别名作字段与数组元素、
// `sizeof`/`len`/`const` 与别名组合。
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
    if len(xs) == 3 {
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
