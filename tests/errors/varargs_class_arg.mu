unit varargs_class_arg;
class C { int x; }
extern int printf(const char* fmt, ...);
int main() {
    C c = new C();
    printf("%d\n", c);
    return 0;
}
