unit ctor_throw_partial;

extern int printf(const char* fmt, ...);

class Boom : Exception {
    Boom(string m) { super(m); }
}

int class_dtor = 0;
int value_dtor = 0;
int base_dtor = 0;
int derived_dtor = 0;

// throws from the constructor body after its reference field was assigned
class Tracked {
    string tag;
    Tracked(string t) {
        tag = t;
        throw new Boom("ctor");
    }
    ~Tracked() { class_dtor += 1; }
}

struct Value {
    int x;
    Value() {
        x = 1;
        throw new Boom("default");
    }
    ~Value() { value_dtor += 1; }
}

class Base {
    string name;
    Base() { name = "base"; }
    ~Base() { base_dtor += 1; }
}

class Derived : Base {
    string extra;
    Derived() {
        super();
        extra = "derived";
        throw new Boom("derived");
    }
    ~Derived() { derived_dtor += 1; }
}

class BaseBoom {
    string name;
    BaseBoom() {
        name = "base";
        throw new Boom("base");
    }
    ~BaseBoom() { base_dtor += 1; }
}

class Derived2 : BaseBoom {
    Derived2() { super(); }
    ~Derived2() { derived_dtor += 1; }
}

class Good {
    int x;
    Good(int v) { x = v; }
    ~Good() { class_dtor += 1; }
}

int main() {
    // class constructor body throws: the object's own destructor must not run. 100 rounds also
    // check that the storage is really freed (a double free or a double field release crashes).
    int i = 0;
    while i < 100 {
        try {
            Tracked t = new Tracked("t");
            printf("unreachable\n");
        } catch (e: Boom) {
        }
        i += 1;
    }
    printf("class_dtor=%d\n", class_dtor);
    if class_dtor != 0 { return 1; }

    // struct with a default constructor, `V v;` form
    try {
        Value a;
        printf("unreachable %d\n", a.x);
    } catch (e: Boom) {
    }
    printf("value_dtor=%d\n", value_dtor);
    if value_dtor != 0 { return 1; }

    // struct constructed in place, `V v = V();` form
    try {
        Value b = Value();
        printf("unreachable %d\n", b.x);
    } catch (e: Boom) {
    }
    printf("value_dtor=%d\n", value_dtor);
    if value_dtor != 0 { return 1; }

    // base constructed, derived constructor body throws: the base destructor runs exactly once
    i = 0;
    while i < 100 {
        try {
            Derived d = new Derived();
            printf("unreachable\n");
        } catch (e: Boom) {
        }
        i += 1;
    }
    printf("base_dtor=%d derived_dtor=%d\n", base_dtor, derived_dtor);
    if base_dtor != 100 or derived_dtor != 0 { return 1; }

    // the base constructor itself throws: the base destructor must not run either
    i = 0;
    while i < 100 {
        try {
            Derived2 d2 = new Derived2();
            printf("unreachable\n");
        } catch (e: Boom) {
        }
        i += 1;
    }
    printf("base_dtor=%d derived_dtor=%d\n", base_dtor, derived_dtor);
    if base_dtor != 100 or derived_dtor != 0 { return 1; }

    // normal construction still runs the destructor exactly once
    {
        Good g = new Good(7);
        if g.x != 7 { return 1; }
    }
    printf("class_dtor=%d\n", class_dtor);
    if class_dtor != 1 { return 1; }

    printf("ctor_throw_partial ok\n");
    return 0;
}
