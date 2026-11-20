unit generic_const_contract_mut;
interface Sized { int size() const; }
struct Mut { int n; Mut() { n = 3; } int size() { n = 5; return n; } }
int probe<T: Sized>(T x) { return x.size(); }
int main() { Mut m = {}; return probe(m); }
