unit generic_duplicate;
T maxv<T>(T a, T b) { return a > b ? a : b; }
T maxv<T>(T a, T b) { return b > a ? b : a; }
int main() { return maxv(1, 2); }
