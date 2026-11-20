unit generic_union_rc;
union U<T> { T x; long y; }
int main() { U<string> u = {}; return 0; }
