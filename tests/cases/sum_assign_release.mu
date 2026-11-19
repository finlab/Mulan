unit sum_assign_release;
use std.cstdio;

// A sum value owns its active payload (SPEC decision #55): assigning over it must release the old
// payload, and a self-assignment must not free the value it is about to store.
int live = 0;

class Obj {
    public int v;
    Obj(int x) { v = x; live += 1; }
    ~Obj() { live -= 1; }
}

variant Holder {
    Holds Obj;
    Empty;
}

int main() {
    Holder h = Holds(new Obj(1));
    if live != 1 {
        return 1;
    }
    h = Holds(new Obj(2));
    if live != 1 {
        printf("assign leaked: %d\n", live);
        return 2;
    }
    h = Empty;
    if live != 0 {
        printf("unit assign leaked: %d\n", live);
        return 3;
    }
    h = Holds(new Obj(3));
    h = h;
    if live != 1 {
        printf("self assign broke: %d\n", live);
        return 4;
    }
    printf("sum assign rc ok\n");
    return 0;
}
