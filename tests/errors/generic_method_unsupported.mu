unit generic_method_unsupported;
struct Box<T> { T v; Box() {} U map<U>(T x) { return x as U; } }
int main() { return 0; }
