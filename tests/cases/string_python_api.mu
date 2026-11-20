// string_python_api — the byte-oriented Python-style API plus the UTF-8 structure helpers.
unit string_python_api;

extern int puts(const char* s);

int main() {
    string s = "  H\u{00E9}LLO  ";        // 2 + (H,é,L,L,O) + 2 code points; é is two bytes
    if s.length() != 10 { puts("length"); return 1; }
    if s.char_count() != 9 { puts("char_count"); return 2; }
    if not s.is_valid_utf8() { puts("valid"); return 3; }
    if s.char_at(2) != 0x48 { puts("char_at H"); return 4; }
    if s.char_at(3) != 0xE9 { puts("char_at e-acute"); return 5; }
    if not s.is_char_boundary(3) { puts("boundary 3"); return 6; }
    if s.is_char_boundary(4) { puts("boundary 4"); return 7; }
    if s.byte_offset(4) != 5 { puts("byte_offset"); return 8; }
    if s.char_index(5) != 4 { puts("char_index"); return 9; }

    int n = 0;
    for c in s.chars() { n += 1; }
    if n != 9 { puts("chars"); return 10; }
    int[] cps = s.uchars();
    if cps.length() != 9 { puts("uchars"); return 11; }

    string t = s.strip_ascii();
    if t != "H\u{00E9}LLO" { puts("strip"); return 12; }
    if t.lower_ascii() != "h\u{00E9}llo" { puts("lower_ascii"); return 13; }
    if t.upper_ascii() != "H\u{00E9}LLO" { puts("upper_ascii"); return 14; }

    string csv = "a,b,,c";
    string[] parts = csv.split(",");
    if parts.length() != 4 { puts("split len"); return 15; }
    if parts[0] != "a" or parts[2].length() != 0 or parts[3] != "c" { puts("split parts"); return 16; }
    string sep = ",";
    if sep.join(parts) != csv { puts("join"); return 17; }

    string rep = "aXbXc";
    if rep.replace("X", "--") != "a--b--c" { puts("replace"); return 18; }
    if rep.replace("Y", "-") != "aXbXc" { puts("replace none"); return 19; }
    if not rep.startswith("aXb") { puts("startswith"); return 20; }
    if not rep.endswith("Xc") { puts("endswith"); return 21; }
    if rep.count("X") != 2 { puts("count"); return 22; }
    if rep.rfind("X") != 3 { puts("rfind"); return 23; }
    if not rep.contains("bX") { puts("contains"); return 24; }

    string ws = "  a  b\tc\n";
    string[] w = ws.split_ascii_whitespace();
    if w.length() != 3 or w[0] != "a" or w[2] != "c" { puts("split whitespace"); return 25; }

    string lines = "a\r\nb\nc";
    string[] ls = lines.splitlines_ascii();
    if ls.length() != 3 or ls[0] != "a" or ls[1] != "b" or ls[2] != "c" { puts("splitlines"); return 26; }

    string abc = "abc";
    string num = "123";
    string uppers = "ABC";
    string spaces = " \t";
    if not abc.is_alpha_ascii() { puts("isalpha ascii"); return 27; }
    if num.is_alpha_ascii() { puts("isalpha digits"); return 28; }
    if not num.is_digit_ascii() { puts("isdigit ascii"); return 29; }
    if not abc.is_lower_ascii() or abc.is_upper_ascii() { puts("islower"); return 30; }
    if not uppers.is_upper_ascii() or uppers.is_lower_ascii() { puts("isupper"); return 31; }
    if not spaces.is_space_ascii() { puts("isspace"); return 32; }
    string e = {};
    if e.is_alpha_ascii() or not e.is_ascii() { puts("empty predicates"); return 33; }

    int[] zcps = new int[2];
    zcps[0] = 0x4E2D; zcps[1] = 0x6587;
    string zh = string(zcps);
    if zh.length() != 6 or zh.char_count() != 2 { puts("from codepoints"); return 34; }
    byte[] raw = new byte[2];
    raw[0] = 0x68; raw[1] = 0x69;
    string hi = string(raw);
    if hi != "hi" { puts("from bytes"); return 35; }

    string ab = "ab";
    if ab.repeat(3) != "ababab" { puts("repeat"); return 36; }
    if ab.remove_prefix("a") != "b" { puts("remove_prefix"); return 37; }
    if ab.remove_suffix("b") != "a" { puts("remove_suffix"); return 38; }

    puts("string python api ok");
    return 0;
}
