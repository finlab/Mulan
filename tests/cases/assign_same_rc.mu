unit assign_same_rc;
use std.cstdio;

// `b = a` when both already hold the same object keeps the count intact: the retain/release pair is
// skipped on the identical-pointer fast path, and a different source still releases the old value.
int live = 0;

class Obj {
    public int v;
    Obj(int x) { v = x; live += 1; }
    ~Obj() { live -= 1; }
}

interface Named {
    int id();
}

class NamedObj : Named {
    public int n;
    NamedObj(int x) { n = x; }
    override int id() { return n; }
}

int main() {
    Obj a = new Obj(1);
    Obj b = new Obj(2);
    if live != 2 {
        return 1;
    }
    b = a;
    if live != 1 {
        printf("different source leaked: %d\n", live);
        return 2;
    }
    b = a;
    if live != 1 {
        printf("same source broke: %d\n", live);
        return 3;
    }
    b = null;
    if live != 1 {
        printf("clearing broke: %d\n", live);
        return 4;
    }
    a = null;

    Named x = new NamedObj(7);
    Named y = x;
    y = x;
    if y.id() != 7 {
        printf("interface assign broke\n");
        return 5;
    }
    printf("assign same ok\n");
    return 0;
}
