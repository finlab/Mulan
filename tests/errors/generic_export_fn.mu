unit generic_export_fn;
export T id<T>(T x) { return x; }
int main() { return id(1); }
