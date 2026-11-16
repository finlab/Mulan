unit delegate_weak_int; type Get = delegate () => int; int main() { int n = 3; Get g = [weak n] () => n; return g(); }
