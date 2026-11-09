unit char_bytes;

extern int puts(readonly char* s);

int main() {
    // char：有符号 8 位（C 风格）
    char a = 'a';
    if a as int != 97 { puts("ascii char fail"); return 1; }
    char neg = 200 as char;          // 0xC8 作为有符号字节 = -56
    if neg as int != -56 { puts("signed char fail"); return 1; }
    char ff = '\xFF';                // 位模式 0xFF = -1
    if ff as int != -1 { puts("char hex literal fail"); return 1; }

    // byte / ubyte / uchar：无符号 8 位（别名，同一类型）
    byte hi = 200;
    if hi as int != 200 { puts("byte unsigned fail"); return 1; }
    byte maxb = 255;
    if maxb as int != 255 { puts("byte max fail"); return 1; }
    char maxc = 127;
    if maxc as int != 127 { puts("char max fail"); return 1; }
    uchar uc = 200;
    if uc as int != 200 { puts("uchar alias fail"); return 1; }
    ubyte ub = hi;                   // byte/ubyte/uchar 可互相赋值
    if ub as int != 200 { puts("ubyte alias fail"); return 1; }

    // 原始字节字面量 + UTF-8 字节视图
    char esc = '\xE4';
    if esc as ubyte != 0xE4 { puts("hex escape fail"); return 1; }
    string s = "中";                 // UTF-8: E4 B8 AD
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
