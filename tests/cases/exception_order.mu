unit exception_order;

extern int puts(const char* s);

class BaseErr : Exception {
    int code;
    BaseErr(string msg, int c) { super(msg); code = c; }
}

class SubErr : BaseErr {
    SubErr(string msg, int c) { super(msg, c); }
}

int main() {
    try {
        throw new SubErr("s", 2);
    } catch (e: SubErr) {
        puts("sub");
    } catch (e: BaseErr) {
        puts("base");
    }

    try {
        throw new BaseErr("b", 1);
    } catch (e: SubErr) {
        puts("wrong");
    } catch (e: BaseErr) {
        puts("base2");
    }
    return 0;
}
