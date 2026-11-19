// exception_uncaught — an exception that reaches `main` prints its unit, type and message to stderr
// and then aborts (exit 3). Without the report the only symptom was a silent exit code 3, which does
// not even distinguish a thrown exception from any other abort.
unit exception_uncaught;

use std.cstdio;
use std.exception;

class MyError : Exception {
    public int code;
    MyError(string msg, int c) : Exception(msg) { code = c; }
}

int main() {
    try {
        throw new MyError("from-main", 9);
    } catch (e: MyError) {
        throw e;               // not caught again: this one aborts
    }
    printf("unreachable\n");
    return 0;
}
