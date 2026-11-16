unit is_null_class;
use std.cstdio;

// `is` is a safe runtime test (SPEC 10.1.2): a null receiver matches nothing and must not be
// dereferenced for the descriptor (the C-shaped lowering used to GEP null + 8 and crash).
class Animal { public int legs; Animal(int n) { legs = n; } }
class Cat : Animal { Cat() { super(4); } }

int main() {
    Animal a = null;
    int hits = 0;
    if a is Cat {
        printf("null matched\n");
        return 1;
    }
    Animal real = new Cat();
    if real is Cat {
        hits += 1;
    }
    if real is Cat c {
        if c != null {
            hits += 1;
        }
    }
    printf("hits=%d\n", hits);
    return 0;
}
