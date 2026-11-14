// sizeof_generic — `sizeof` / `alignof` accept generic instantiations (including ones imported from
// another unit) and anonymous delegate types, while `sizeof(expr)` still parses as an expression.
unit sizeof_generic;

use std.cstdio;
use std.interfaces;

type Cb = (int) => int;

struct Box<T> {
    public T item;
}

int main() {
    // an instantiated generic interface from another unit: 16 bytes, aligned to 8
    if (sizeof(IIterator<int>) as int) != 16 { printf("sizeof iface\n"); return 1; }
    if (alignof(IIterator<int>) as int) != 8 { printf("alignof iface\n"); return 2; }

    // a local generic instantiation and an anonymous delegate type
    if (sizeof(Box<int>) as int) != 4 { printf("sizeof box\n"); return 3; }
    if (sizeof(delegate (int) => int) as int) != 16 { printf("sizeof delegate\n"); return 4; }
    if (sizeof(Cb) as int) != 8 { printf("sizeof fn\n"); return 5; }

    // expressions stay expressions: an element, an array, and a `<` comparison
    int[3] arr = { 1, 2, 3 };
    int x = 3;
    int y = 4;
    if (sizeof(arr[0]) as int) != 4 { printf("sizeof elem\n"); return 6; }
    if (sizeof(arr) as int) != 12 { printf("sizeof array\n"); return 7; }
    if (sizeof(x < y) as int) != 1 { printf("sizeof comparison\n"); return 8; }

    printf("sizeof generic ok\n");
    return 0;
}
