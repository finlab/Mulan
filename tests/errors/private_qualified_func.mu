unit private_qualified_func;
use privfunc.lib;
type F = (int) => int;
int main() { F g = privfunc.lib.secv; return g(1); }
