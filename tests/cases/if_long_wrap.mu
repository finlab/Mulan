// A `long` sum wraps at 64 bits, and the fold must not invoke the compiler's own undefined behaviour:
// `MAX + 1` is the compiler evaluating a program it then rejects or accepts (#70).
unit if_long_wrap;

extern int puts(const char* s);

const long MAX = 9223372036854775807;
const long MIN = -9223372036854775807 - 1;

#if MAX + 1 == MIN {
int main() { puts("long wrap"); return 0; }
}
#else {
int main() { puts("long no-wrap"); return 1; }
}
