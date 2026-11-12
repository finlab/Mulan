unit exception_rethrow;

extern int printf(const char* fmt, ...);

class BaseErr : Exception {
    int code;
    BaseErr(string msg, int c) { super(msg); code = c; }
}

int main() {
    try {
        throw new BaseErr("a", 5);
    } catch (e: BaseErr) {
        try {
            throw e;
        } catch (e2: BaseErr) {
            printf("re %d\n", e2.code);
        }
    }
    return 0;
}
