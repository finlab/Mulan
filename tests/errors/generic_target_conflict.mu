unit generic_target_conflict;
struct Pair<T, U> { T a; U b; }
Pair<T, int> pack<T>(T x) {
    Pair<T, int> p = {};
    p.a = x;
    return p;
}
int main() { Pair<string, int> p = pack(1); return 0; }
