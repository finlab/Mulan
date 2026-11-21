// exception_messages — runtime cold-path exceptions carry index/length and operation context (#245).
unit exception_messages;

extern int puts(const char* s);

int main() {
    int ok = 0;
    try {
        int[] a = new int[3];
        int i = 7;
        int x = a[i];
    } catch (e: BoundsError) {
        if e.message.contains("index 7") and e.message.contains("length 3") { ok += 1; }
    }
    try {
        int z = 0;
        int q = 1 / z;
    } catch (e: ArithmeticError) {
        if e.message.contains("division by zero") { ok += 1; }
    }
    try {
        long one = 1;
        long c = 64;
        long q = one << c;
    } catch (e: NumericEdgeError) {
        if e.message.contains("shift count 64") { ok += 1; }
    }
    if ok != 3 { puts("messages"); return 1; }
    puts("exception messages ok");
    return 0;
}
