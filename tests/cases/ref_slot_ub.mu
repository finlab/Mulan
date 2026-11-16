// ref_slot_ub — regression for 0.0.144: the slot of a `ref` / `move` parameter must be pointer sized.
//
// Before the fix, `bindParams` allocated the slot with the *referent* type, so storing the incoming
// pointer wrote 8 bytes into e.g. a 4-byte alloca. That out-of-bounds store is UB, and the optimizer
// used the license: in one program it deleted a move constructor call and folded a later
// `if live != 0` to "always true". The values below are what a caller sees when the slots are correct.
unit ref_slot_ub;

extern int printf(const char* fmt, ...);

int live = 0;

struct Small { int v; }          // 4 bytes: the old slot was too small for the pointer

struct Res {
    int* p;
    Res() { p = null; live = live + 1; }
    ~Res() { free(p); live = live - 1; }
}

struct Val {
    int v;
    Val() { v = 41; }
    Val(move Val o) { v = o.v; }
}

void bump(ref Small s) { s.v = s.v + 1; }

void bump_readonly(const ref Small s, int* out) { out[0] = s.v; }

int main() {
    // 1. a `ref` parameter of a 4-byte struct
    {
        Small s;
        s.v = 7;
        bump(s);
        if s.v != 8 { printf("ref struct %d\n", s.v); return 1; }

        int seen = 0;
        bump_readonly(s, &seen);
        if seen != 8 { printf("const ref %d\n", seen); return 2; }
    }

    // 2. `for ref` bindings use the same slot sizing (the loop variable is a borrow of an element)
    {
        Small[] arr = new Small[2];
        arr[0].v = 1;
        arr[1].v = 2;
        for ref e in arr { e.v = e.v + 10; }
        if arr[0].v != 11 or arr[1].v != 12 { printf("for ref %d %d\n", arr[0].v, arr[1].v); return 3; }
    }

    // 3. the move constructor takes its source through a pointer slot
    {
        Val a;
        Val b = move a;
        if b.v != 41 { printf("move ctor %d\n", b.v); return 5; }
    }

    // 4. destructors next to those slots still run exactly once
    {
        Res r;
        if live != 1 { printf("live %d\n", live); return 6; }
    }
    if live != 0 { printf("leak %d\n", live); return 7; }

    printf("ref_slot_ub ok\n");
    return 0;
}

