unit audit211_generic_base_spelling;
class Base<T> { public T v; Base(T x) { v = x; } }
class Derived<T> : Base<T> {
public:
    Derived(T a) : Base<T>(a) { }
}
int main() { return 0; }
