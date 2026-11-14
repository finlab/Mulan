unit sum_match_missing;
type V = A(int) | B(const char*);
int main() {
    V v = V.A(1);
    match v { A(n): { } }
    return 0;
}
