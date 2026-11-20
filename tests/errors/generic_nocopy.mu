unit generic_nocopy;
nocopy struct NC<T> { T v; NC() {} }
int main() { NC<int> a = {}; NC<int> b = a; return 0; }
