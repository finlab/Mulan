unit e5; int main() { int[3] a = {1,2,3}; const int[] rs = a; for ref v in rs { v = 2; } return 0; }
