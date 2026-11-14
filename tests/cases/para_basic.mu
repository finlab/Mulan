// para_basic — two sections run concurrently and join; results go through a shared class
// (writing an enclosing local is a compile error), and the enclosing local `n` is captured.

unit para_basic;

use std.cstdio;

class Box {
    public int a;
    public int b;
}

int work(int n) {
    int s = 0;
    for i in 0..n {
        s = s + i;
    }
    return s;
}

int main() {
    Box out = new Box();
    int n = 1000;
    para {
        { out.a = work(n); }
        { out.b = work(n * 2); }
    }
    printf("%d %d\n", out.a, out.b);
    return 0;
}
