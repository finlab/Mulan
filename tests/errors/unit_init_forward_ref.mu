unit unit_init_forward_ref;

int f() { return 2; }

int a = b + 1;   // `b` is initialized after `a`
int b = f();

int main() { return 0; }
