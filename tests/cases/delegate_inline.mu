// delegate_inline — an immediately-invoked capturing lambda keeps its environment on the stack
// (U41 D5): no allocation, and the captured RC fields are destroyed right after the call.
unit delegate_inline;

use std.cstdio;
use std.string;

int live = 0;

class Box {
    public int id;
    Box(int x) {
        id = x;
        live = live + 1;
    }
    ~Box() { live = live - 1; }
}

int main() {
    int x = 7;
    if ([x] (int a) => a + x)(3) != 10 { printf("scalar\n"); return 1; }

    String s = "hello";
    if ([s] () => s.length() as int)() != 5 { printf("string\n"); return 2; }

    // the environment's own reference to the object is released after the call
    Box b = new Box(1);
    if ([b] () => b.id)() != 1 { printf("class\n"); return 3; }
    b = null;

    int y = 2;
    if ([x, y] (int a) => a * x + y)(4) != 30 { printf("two captures\n"); return 4; }

    // nested: the inner lambda is immediately invoked inside the outer body
    if ([x, y] () => ([y] (int a) => a + y)(x))() != 9 { printf("nested\n"); return 5; }

    printf("delegate inline ok\n");
    return 0;
}
