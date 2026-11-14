unit struct_iface_default_conflict;

interface A { void hi() { } }
interface B { void hi() { } }

struct S : A, B { }

int main() { return 0; }
