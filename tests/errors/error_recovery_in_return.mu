unit error_recovery_in_return;
int badfn() { return 1.5; }
int arrfn() { int[3] a; int[3] b; a = b; return a[0]; }
int main() { return 0; }
