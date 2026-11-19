unit ternary_copy;

// `?:` yields one owned value. A borrowed branch of a type with a user copy constructor is copied
// through that constructor exactly once (C++ semantics: the branch is an lvalue, so the result is a
// copy), and taking the result over (declaration initializer or assignment) copies it no further and
// leaks no reference count. A type without a copy constructor keeps the field-level retain.

use std.cstdio;
use std.string;

int g_live = 0;
int g_copies = 0;

struct Box {
    string tag;
    Box(const char* t) { tag = string(t); g_live = g_live + 1; }
    Box(const ref Box o) { tag = o.tag; g_live = g_live + 1; g_copies = g_copies + 1; }
    ~Box() { g_live = g_live - 1; }
}

struct Tag {
    string name;
}

int main() {
    Box b1 = Box("left");
    Box b2 = Box("right");
    bool c = true;

    Box a = (c ? b1 : b2);
    printf("%s live=%d copies=%d\n", a.tag.cstr(), g_live, g_copies);

    c = false;
    a = (c ? b1 : b2);
    printf("%s live=%d copies=%d\n", a.tag.cstr(), g_live, g_copies);

    c = true;
    int i = 0;
    while (i < 1000) {
        a = (c ? b1 : b2);
        i = i + 1;
    }
    printf("%s live=%d copies=%d\n", a.tag.cstr(), g_live, g_copies);

    Tag t1;
    t1.name = string("one");
    Tag t2;
    t2.name = string("two");
    Tag picked = (c ? t1 : t2);
    printf("%s %s %s\n", picked.name.cstr(), t1.name.cstr(), t2.name.cstr());
    return 0;
}
