unit generic_nomove;
nomove struct NM<T> { T v; NM() {} }
int main() { NM<int> a = {}; NM<int> b = move a; return 0; }
