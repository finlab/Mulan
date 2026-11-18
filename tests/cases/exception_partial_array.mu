unit exception_partial_array;

extern int printf(const char* fmt, ...);

class Boom : Exception {
    Boom(string m) : Exception(m) { }
}

// #26: a fixed array whose constructor throws part way builds a prefix. The elements before it are
// complete and get their full destructor in reverse order; the throwing element never completed and
// must not run its user destructor body.
int attempts = 0;
int dtors = 0;
int order0 = 0;
int order1 = 0;

struct Item {
    int marker;
    Item() {
        attempts += 1;
        marker = attempts;
        if attempts == 3 { throw new Boom("third"); }
    }
    ~Item() {
        if marker != 0 {
            if dtors == 0 { order0 = marker; } else { order1 = marker; }
            dtors += 1;
        }
    }
}

// An array of structs with no user constructor anywhere: the whole tree is one inline construction
// sequence, so the interrupted element and its enclosing level are released by the same rule.
int leaf_attempts = 0;
int leaf_ctors = 0;
int leaf_dtors = 0;

struct Leaf {
    int marker;
    Leaf() {
        leaf_attempts += 1;
        marker = leaf_attempts;
        if leaf_attempts == 2 { throw new Boom("leaf"); }
        leaf_ctors += 1;
    }
    ~Leaf() {
        if marker != 0 { leaf_dtors += 1; }
    }
}

struct Tree {
    Leaf leaf;
    int pad;
}

int main() {
    try { Item[3] items; } catch (e: Boom) { }
    printf("attempts=%d dtors=%d order=%d,%d\n", attempts, dtors, order0, order1);
    if attempts != 3 or dtors != 2 or order0 != 2 or order1 != 1 { return 1; }

    try { Tree[4] forest; } catch (e: Boom) { }
    printf("leaf_attempts=%d leaf_ctors=%d leaf_dtors=%d\n", leaf_attempts, leaf_ctors, leaf_dtors);
    if leaf_attempts != 2 or leaf_ctors != 1 or leaf_dtors != 1 { return 1; }

    printf("exception_partial_array ok\n");
    return 0;
}
