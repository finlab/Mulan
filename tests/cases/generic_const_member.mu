// generic_const_member — type-level constants inside a generic struct/class are substituted and
// resolved per instance (method access and instance access).
unit generic_const_member;

extern int puts(const char* s);

struct KB<T> {
    const int K = 3;
    T v;
    KB() {}
    int k() const { return K; }
}

class KC<T> {
    public const int K = 4;
    public T v;
    int k() const { return K; }
}

int main() {
    KB<int> kb = {};
    if kb.k() != 3 or kb.K != 3 { puts("struct const"); return 1; }
    KC<int> kc = new KC<int>();
    if kc.k() != 4 or kc.K != 4 { puts("class const"); return 2; }
    puts("generic const member ok");
    return 0;
}
