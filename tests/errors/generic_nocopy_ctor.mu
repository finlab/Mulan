unit generic_nocopy_ctor;
nocopy struct NC<T> { T v; NC() {} NC(const ref NC<T> o) { v = o.v; } }
int main() { NC<int> a = {}; return 0; }
