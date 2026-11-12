unit exception_nested;

extern int puts(const char* s);

class BaseErr : Exception {
    BaseErr(string msg) { super(msg); }
}

class OtherErr : Exception {
    OtherErr(string msg) { super(msg); }
}

int main() {
    try {
        try {
            throw new BaseErr("x");
        } catch (e: OtherErr) {
            puts("inner");
        }
        puts("missed");
    } catch (e: BaseErr) {
        puts("outer");
    }
    return 0;
}
