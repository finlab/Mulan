unit delegate_weak_ref_order; type G = delegate () => int; class C { public int v; } int main() { C c = new C(); G g = [ref weak c] () => 1; return g(); }
