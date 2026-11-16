unit delegate_weak_struct_this; type Get = delegate () => int; struct S { int v; void f() { Get g = [weak this] () => v; } } int main() { return 0; }
