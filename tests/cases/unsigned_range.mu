// unsigned_range — an unsigned range for-in compares unsigned (#205).
unit unsigned_range;

extern int puts(const char* s);

int main() {
    int n = 0;
    for i in 0u..3000000000u { n += 1; }
    if n != 3000000000 as int { puts("iterations"); return 1; }
    puts("unsigned range ok");
    return 0;
}
