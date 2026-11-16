unit mv_arg;
nocopy struct Buf { int* p; Buf(int n) { p = alloc(n) as int*; } ~Buf() { free(p); } Buf(move Buf o) { p = o.p; } }
int take(Buf b) { return 1; }
int main() { Buf c = Buf(4); return take(c); }
