unit audit211_field_release;
extern int puts(const char* s);
int live = 0;

struct Probe {
    int id;
    Probe() { id = live; live += 1; }
    ~Probe() { live -= 1; }
}

// Plain constructor: the member step builds `p`, the body throws, `p` must still be released exactly once.
class Plain {
    Probe p;
    Plain(int x) { if x > 0 { throw new Exception("plain"); } }
}

// Delegating constructor: the target builds everything; a throw in the delegating body must release the
// same state exactly once (the target's own sequence already closed).
class Delegated {
    Probe p;
    Delegated(int x) { if x > 0 { throw new Exception("delegated"); } }
    Delegated() : Delegated(1) { }
}

int main() {
    try { Plain a = new Plain(1); } catch (e: Exception) { }
    if live != 0 { puts("plain leaked a field"); return 1; }

    try { Delegated b = new Delegated(); } catch (e: Exception) { }
    if live != 0 { puts("delegated leaked a field"); return 2; }

    puts("field release ok");
    return 0;
}
