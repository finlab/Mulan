unit sum_match_arity;
type V = A(int) | B(const char*);
int main() {
    V v = V.A(1);
    switch v { A(n, k): { } B: { } }
    return 0;
}
