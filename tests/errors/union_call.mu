unit union_call;
union U { int i; float f; }
int main() { U u = U(1); return u.i; }
