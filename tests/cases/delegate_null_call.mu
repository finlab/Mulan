// delegate_null_call — calling a null delegate raises the catchable NullReferenceError, not a jump to
// address zero (#243).
unit delegate_null_call;

extern int puts(const char* s);

type F = delegate (int) => int;

int main() {
    F f = null;
    int caught = 0;
    try {
        int r = f(1);
    } catch (e: Exception) {
        caught += 1;
    }
    if caught != 1 { puts("delegate null"); return 1; }
    puts("delegate null call ok");
    return 0;
}
