// unicode_extension — the Unicode method names appear only when std.unicode is loaded.
unit unicode_extension;

use std.unicode;

extern int puts(const char* s);

int main() {
    string s = "  H\u{00E9}LLO  ";
    if s.lower() != "  h\u{00E9}llo  " { puts("lower"); return 1; }
    if s.upper() != "  H\u{00C9}LLO  " { puts("upper"); return 2; }
    if s.strip() != "H\u{00E9}LLO" { puts("strip"); return 3; }
    if s.isalpha() { puts("isalpha spaces"); return 4; }
    if not s.strip().isalpha() { puts("isalpha word"); return 5; }

    string ru = "\u{041F}\u{0440}\u{0438}\u{0432}\u{0435}\u{0442}";
    if ru.lower() != "\u{043F}\u{0440}\u{0438}\u{0432}\u{0435}\u{0442}" { puts("cyrillic lower"); return 6; }
    if not ru.isalpha() { puts("cyrillic alpha"); return 7; }

    string zh = "\u{4E2D}\u{6587}";
    if not zh.isalpha() { puts("han alpha"); return 8; }
    if zh.isdigit() { puts("han digit"); return 9; }

    string sp = "\u{3000}hi\u{3000}";
    if sp.strip() != "hi" { puts("ideographic space"); return 10; }

    string ar = "\u{0661}\u{0662}\u{0663}";
    if not ar.isdigit() { puts("arabic digits"); return 11; }

    string e = {};
    if e.isalpha() or e.isspace() or e.isdigit() { puts("empty predicates"); return 12; }

    puts("unicode extension ok");
    return 0;
}
