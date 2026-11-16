unit delegate_weak_ref; type Get = delegate () => int; class C { public int v; } int main() { C c = new C(); Get g = [weak ref c] () => 1; return g(); }
