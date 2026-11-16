unit generic_inst_chain;

T pick<T>(T x) { return x.n; }

T outer<T>(T x) { return pick<T>(x); }

int main() { return outer<int>(5); }
