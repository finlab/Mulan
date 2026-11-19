unit extend_abstract_added;
interface I { int f(); }
interface J : I { int g(); }
extend I with J;
int main() { return 0; }
