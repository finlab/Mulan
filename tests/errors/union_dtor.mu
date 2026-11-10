unit union_dtor;
union U { int i; ~U() { i = 0; } }
int main() { return 0; }
