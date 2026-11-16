unit string_bounds;

extern int puts(const char* s);

// #32/#33/#34: `sub` clamps its range and copies into a NUL-terminated buffer, `cstr()` represents
// the zero value as the empty C string, and `find` returns an offset wide enough for 64-bit sizes.
int main() {
    string s = "abcdef";

    string t = s.sub(2, 3);
    if t != "cde" or t.length() != 3 { return 1; }
    if t.cstr()[3] != 0 { return 2; }

    // count past the end is clamped, not turned into a view past the buffer
    string over = s.sub(4, 10);
    if over != "ef" or over.length() != 2 { return 3; }

    // start past the end is the empty string
    string past = s.sub(10, 2);
    if past.length() != 0 or past != "" { return 4; }
    if past.cstr() == null or past.cstr()[0] != 0 { return 5; }

    // the zero value is the empty C string, not a null pointer
    string z = {};
    if z.cstr() == null { return 6; }
    if z.cstr()[0] != 0 { return 7; }
    if z.sub(0, 5).length() != 0 { return 8; }

    // offsets are `isize`, so a valid match is never confused with -1
    isize pos = s.find("cde");
    if pos != 2 { return 9; }
    if s.find("zz") != -1 { return 10; }

    // trimming still produces a NUL-terminated value
    string padded = "  hi  ";
    string trimmed = padded.trim();
    if trimmed != "hi" or trimmed.cstr()[2] != 0 { return 11; }

    puts("string bounds ok");
    return 0;
}
