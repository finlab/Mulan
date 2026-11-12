unit forin_index;

extern int puts(const char* s);

// index form: length() -> usize + operator[](C, usize)
struct Vec {
    int[4] data;
    usize n;
    usize length() { return n; }
}

int operator[](Vec v, usize i) { return v.data[i]; }

class List {
    int[4] data;
    usize n;
    List() { n = 0; }
    usize length() { return n; }
}

int operator[](List l, usize i) { return l.data[i]; }

Vec make() {
    Vec v = {};
    v.data[0] = 1;
    v.data[1] = 2;
    v.data[2] = 3;
    v.n = 3;
    return v;
}

int main() {
    Vec v = {};
    v.data[0] = 10;
    v.data[1] = 11;
    v.data[2] = 12;
    v.n = 3;

    int sum = 0;
    for x in v { sum += x; }
    if sum != 33 { puts("struct sum failed"); return 1; }

    int weighted = 0;
    for i, x in v { weighted += (i as int) * x; }
    if weighted != 35 { puts("struct index failed"); return 1; }

    List l = new List();
    l.data[0] = 1;
    l.data[1] = 2;
    l.data[2] = 3;
    l.data[3] = 4;
    l.n = 4;

    int s2 = 0;
    for x in l { s2 += x; }
    if s2 != 10 { puts("class sum failed"); return 1; }

    int s3 = 0;
    for i, x in l { s3 += i as int; }
    if s3 != 6 { puts("class index failed"); return 1; }

    // i for builtins: real index for arrays, offset from lo for ranges
    int[3] a = {5, 6, 7};
    int bi = 0;
    for (i, x in a) { bi += (i as int) * x; }
    if bi != 20 { puts("array index failed"); return 1; }

    // `for i, ref x`: index + reference binding
    for i, ref x in a { x += i as int; }
    if a[0] != 5 or a[1] != 7 or a[2] != 9 { puts("array ref index failed"); return 1; }

    int ri = 0;
    for i, x in 10..13 { ri += (i as int) + x; }
    if ri != 36 { puts("range index failed"); return 1; }

    // empty container: zero iterations
    Vec e = {};
    int c = 0;
    for i, x in e { c += 1; }
    if c != 0 { puts("empty failed"); return 1; }

    // temporary container evaluated only once
    int t = 0;
    for x in make() { t += x; }
    if t != 6 { puts("temp failed"); return 1; }

    l = null;
    puts("forin index ok");
    return 0;
}
