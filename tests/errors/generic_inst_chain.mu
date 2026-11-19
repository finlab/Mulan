unit generic_inst_chain;

T pick<T>(T x) { return x + x; }

T outer<T>(T x) { return pick<T>(x); }

int main() { return outer<bool>(true); }
