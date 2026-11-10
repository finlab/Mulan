unit extern_overload;
extern int puts(const char* s);
extern int getv(int a);
int getv(long a) { return 1; }
int main() { return 0; }
