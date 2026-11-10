unit structs2;

extern int puts(const char* s);

struct Inner { int a, b; }
struct Outer { Inner inner; string name; int[3] nums; }

int sum(Inner x) { return x.a + x.b; }

int main() {
    Outer o = {};
    o.inner.a = 1;
    o.inner.b = 2;
    o.name = "hi";
    o.nums[0] = 7;

    Outer p = o;
    p.inner.a = 9;
    if o.inner.a == 1 and p.inner.a == 9 { puts("nested copy ok"); }
    if sum(o.inner) == 3 { puts("nested pass ok"); }
    if o.name == "hi" and len(o.name) == 2 { puts("string field ok"); }
    if p.nums[0] == 7 { puts("array field copy ok"); }

    for ref v in o.nums {
        v += 1;
    }
    if o.nums[0] == 8 and p.nums[0] == 7 { puts("array field ref ok"); }

    return 0;
}
