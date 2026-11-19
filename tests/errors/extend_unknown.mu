unit extend_unknown;
interface I { int f(); }
extend I with Missing;
int main() { return 0; }
