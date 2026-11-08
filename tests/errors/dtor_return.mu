unit dtor_return;
class C { int v; C(){v=0;} ~C() { return; } }
int main() { C c = new C(); del c; return 0; }
