// string_utf8_boundary — byte offsets and character boundaries agree on malformed UTF-8: an invalid
// byte is its own U+FFFD (the lossy model of SPEC 6.3) (#230).
unit string_utf8_boundary;

extern int puts(const char* s);

int main() {
    byte[] b = new byte[3];
    b[0] = 0xC3;
    b[1] = 0x80;
    b[2] = 0x80;
    string s = string(b);
    if s.char_count() != 2 { puts("count"); return 1; }
    if s.byte_offset(1) != 2 { puts("offset"); return 2; }
    if not s.is_char_boundary(2) { puts("boundary"); return 3; }
    if s.char_index(2) != 1 { puts("index"); return 4; }
    puts("string utf8 boundary ok");
    return 0;
}
