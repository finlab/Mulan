unit generic_target_conflict;
struct Pair<T, U> { T a; U b; }
Pair<T, T> make2<T>() {
    Pair<T, T> p = {};
    return p;
}
int main() { Pair<int, long> p = make2(); return 0; }
