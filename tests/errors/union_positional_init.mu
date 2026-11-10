unit union_positional_init;
union U { int i; float f; }
int main() { U u = { 1 }; return u.i; }
