unit char_bytes;
extern int puts(readonly char* s);
int main() {
    char a = 'a';
    if a as int != 97 { puts("ascii byte fail"); return 1; }
    char hi = 200;                 // char 是 u8：200 合法
    if hi as int != 200 { puts("unsigned fail"); return 1; }
    char esc = '\xE4';             // 原始字节字面量
    if esc as ubyte != 0xE4 { puts("hex escape fail"); return 1; }
    string s = "中";               // UTF-8: E4 B8 AD
    if len(s) != 3 { puts("string bytes fail"); return 1; }
    if s[0] as ubyte != 0xE4 { puts("first byte fail"); return 1; }
    if s[1] as ubyte != 0xB8 { puts("second byte fail"); return 1; }
    if s[2] as ubyte != 0xAD { puts("third byte fail"); return 1; }
    int n = 0;
    for c in s { n += 1; }
    if n != 3 { puts("byte iteration fail"); return 1; }
    puts("char bytes ok");
    return 0;
}

