// ref_param.mu — `ref` parameters: mutable borrow and read-only borrow (`ref const T p`)
unit ref_param;

extern int puts(const char* s);

struct Point {
    int x, y;
}

void bump(ref Point p) {
    p.x += 1;
    p.y += 1;
}

void peek(ref const Point p) {
    int v = p.x;
    v += p.y;
    if v == 5 {
        puts("const ref read ok");
    }
}

int total(ref const Point p) {
    return p.x + p.y;
}

int main() {
    Point a = {1, 2};
    bump(a);
    if a.x == 2 and a.y == 3 {
        puts("ref write ok");
    }
    peek(a);
    if total(a) == 5 {
        puts("const ref call ok");
    }
    bump(a);
    if total(a) == 7 {
        puts("ref again ok");
    }

    Point[2] arr = {{10, 20}, {30, 40}};
    bump(arr[1]);
    if arr[1].x == 31 and arr[1].y == 41 {
        puts("array elem ref ok");
    }

    Point b = {5, 6};
    int s = total(b);
    if s == 11 {
        puts("value call ok");
    }
    return 0;
}
