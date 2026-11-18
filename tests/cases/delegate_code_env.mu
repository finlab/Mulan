// delegate_code_env — `d.code` / `d.env` are the two words of a delegate value as `void*`
// (SPEC 3.16.1a): the C interoperability escape hatch when an `extern` signature cannot carry a
// delegate. A captureless delegate has a null environment.
unit delegate_code_env;

use std.cstdio;

type Op = delegate (int) => int;

int triple(int x) { return x * 3; }

int main() {
    Op f = triple;
    void* code = f.code;
    void* env = f.env;
    if code == null { printf("code null\n"); return 1; }
    if not (env == null) { printf("captureless env not null\n"); return 2; }
    if f(5) != 15 { printf("call\n"); return 3; }

    int v = 4;
    Op g = [v] (int x) => x + v;
    if g.code == null { printf("capture code null\n"); return 4; }
    if g.env == null { printf("capture env null\n"); return 5; }

    // two delegates to the same function share the code word
    Op h = triple;
    if h.code != f.code { printf("code differs\n"); return 6; }

    printf("delegate code env ok\n");
    return 0;
}
