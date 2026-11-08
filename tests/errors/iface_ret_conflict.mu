unit iface_ret_conflict;
interface I1 { int f(); }
interface I2 { bool f(); }
class C : I1, I2 { override int f() { return 1; } }
int main() { return 0; }
