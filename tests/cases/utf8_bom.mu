// utf8_bom — a leading UTF-8 BOM is metadata, not an invalid character (#139).
unit utf8_bom;

extern int puts(const char* s);

int main() {
    puts("utf8 bom ok");
    return 0;
}
