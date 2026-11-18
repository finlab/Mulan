unit move_temporary; nocopy struct B { int* p; B(int n) { p = alloc(n) as int*; } ~B() { free(p); } B(move B o) { p = o.p; } } int main() { B a = B(1); a = move B(2); return 0; }
