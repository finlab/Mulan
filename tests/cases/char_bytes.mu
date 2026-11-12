unit char_bytes;

extern int puts(const char* s);

int main() {
    // char: signed 8-bit (C style)
    char a = 'a';
    if a as int != 97 { puts("ascii char fail"); return 1; }
    char neg = 200 as char;          // 0xC8 as a signed byte = -56
    if neg as int != -56 { puts("signed char fail"); return 1; }
    char ff = '\xFF';                // bit pattern 0xFF = -1
    if ff as int != -1 { puts("char hex literal fail"); return 1; }
    char maxc = 127;
    if maxc as int != 127 { puts("char max fail"); return 1; }

    // byte: unsigned 8-bit (the only unsigned 8-bit type)
    byte hi = 200;
    if hi as int != 200 { puts("byte unsigned fail"); return 1; }
    byte maxb = 255;
    if maxb as int != 255 { puts("byte max fail"); return 1; }
    byte from_lit = 'a';             // char literal adapts to the context
    if from_lit as int != 97 { puts("byte literal adapt fail"); return 1; }

    // char literal: defaults to byte; it adapts when compared with char
    if '\xE4' != 228 { puts("char literal default fail"); return 1; }
    char ca = 'a';
    if ca != 'a' { puts("char compare adapt fail"); return 1; }

    // string is a UTF-8 byte view: s[i] and for-in both work on bytes
    string s = "中";                 // UTF-8: E4 B8 AD
    if s.length() != 3 { puts("string bytes fail"); return 1; }
    if s[0] != 0xE4 { puts("first byte fail"); return 1; }
    if s[1] != 0xB8 { puts("second byte fail"); return 1; }
    if s[2] != 0xAD { puts("third byte fail"); return 1; }
    int n = 0;
    for c in s {
        if c > 255 { puts("loop byte range fail"); return 1; }
        n += 1;
    }
    if n != 3 { puts("byte iteration fail"); return 1; }

    puts("char bytes ok");
    return 0;
}
