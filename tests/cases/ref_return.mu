// ref_return.mu — `ref T` 返回类型、借用下标、借用接收者、for ref
unit ref_return;

extern int puts(const char* s);

struct Vec {
    usize n;
    int* data;

    usize length() { return n; }
    ref int at(usize i) { return data[i]; }
}

// 借用接收者 + 借用返回：写穿调用方的存储（内联存储容器也能写）
struct Small {
    int[4] v;

    usize length() { return 4; }
    ref int at(usize i) { return v[i]; }
}

ref int operator[](ref Vec v, usize i) { return v.data[i]; }
ref int operator[](ref Small s, usize i) { return s.v[i]; }
ref const int peek(ref const Vec v, usize i) { return v.data[i]; }

void bump(ref int x) { x += 1; }

struct P { int x, y; }

struct PVec {
    usize n;
    P* data;

    usize length() { return n; }
}

ref P operator[](ref PVec v, usize i) { return v.data[i]; }

int main() {
    int[4] a = {1, 2, 3, 4};
    Vec v = {4, &a[0]};

    v[1] = 20;
    if a[1] == 20 {
        puts("ref index write ok");
    }

    v[1] += 5;
    if a[1] == 25 {
        puts("ref index compound ok");
    }

    bump(v[2]);
    if a[2] == 4 {
        puts("pass element to ref param ok");
    }

    int copy = v[0];
    if copy == 1 {
        puts("ref index read ok");
    }

    v.at(3) = 40;
    if a[3] == 40 {
        puts("method ref return ok");
    }

    if peek(v, 0) == 1 {
        puts("ref const read ok");
    }

    for ref e in v {
        e += 1;
    }
    if a[0] == 2 and a[3] == 41 {
        puts("for ref over user type ok");
    }

    Small s = {};
    s[2] = 7;
    if s.at(2) == 7 {
        puts("inline storage ref receiver ok");
    }

    P[2] arr = {{1, 2}, {3, 4}};
    PVec pv = {2, &arr[0]};
    pv[1].x = 30;
    pv[0] = P(10, 20);
    int sum = 0;
    for e in pv {
        sum += e.x;
    }
    if arr[1].x == 30 and arr[0].x == 10 and sum == 40 {
        puts("struct element borrow ok");
    }

    return 0;
}
