unit exception_basic;

extern int printf(const char* fmt, ...);

class MyError : Exception {
    int code;
    MyError(string msg, int c) { super(msg); code = c; }
}

int f(int x) {
    if x < 0 { throw new MyError("bad", x); }
    return x;
}

int main() {
    try {
        int v = f(-7);
        printf("no %d\n", v);
    } catch (e: MyError) {
        printf("%d %d\n", e.code, e.message.length() as int);
    }
    return 0;
}
