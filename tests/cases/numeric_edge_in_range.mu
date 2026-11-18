// The checks only reject what the conversion cannot represent: in-range values behave as before, and an
// unsigned target takes the unsigned conversion (`200.0 as byte` stays 200 rather than a poison i8).
unit numeric_edge_in_range;

extern int printf(const char* fmt, ...);

int main() {
    double d = 200.0;
    byte b = d as byte;
    ulong u = 1.0e18 as ulong;
    int n = 3;
    printf("%d %d %d\n", b as int, (u / 1000000000000000000 as ulong) as int, 1 << n);
    return 0;
}
