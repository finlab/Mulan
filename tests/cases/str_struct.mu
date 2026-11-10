unit str_struct;
extern int puts(const char* s);

string G = "global";

struct Box {
    string s;
    int tag;
}

class Holder {
    string name;
}

Box mk() {
    Box b = {"made", 7};
    return b;
}

int main() {
    // 字面量/长度/下标/字节遍历/值比较
    string s = "abc";
    if len(s) != 3 { return 1; }
    if s[0] != 'a' or s[2] != 'c' { return 2; }
    if s != "abc" { return 3; }
    if s == "abd" { return 4; }
    if len("abc") != 3 or "abc"[1] != 'b' { return 5; }
    int n = 0;
    for c in s { if c != 0 { n += 1; } }
    if n != 3 { return 6; }
    for c in "xyz" { n += 1; }
    if n != 6 { return 7; }

    // 零值 / 方法
    string z = {};
    if len(z) != 0 { return 8; }
    if not z.empty() { return 9; }
    if s.empty() { return 10; }
    if s.length() != 3 { return 11; }
    if s.at(1) != 'b' { return 12; }

    // 拼接 / 子串 / c_str / from_cstr
    string t = s.concat("de");
    if t != "abcde" { return 13; }
    if t[4] != 'e' { return 14; }
    if t.cstr()[5] != 0 { return 15; }
    string u = s + "xyz";
    if u.length() != 6 or u != "abcxyz" { return 16; }
    string sub = u.sub(3, 3);
    if sub != "xyz" { return 17; }
    if len(sub) != 3 { return 18; }
    string f = string("cstr");
    if f != "cstr" { return 19; }

    // struct 字段 / 拷贝 / 返回 / class 字段
    Box b = {"box", 1};
    if b.s != "box" { return 20; }
    Box b2 = b;
    if b2.s != "box" or b2.tag != 1 { return 21; }
    Box m = mk();
    if m.s != "made" or m.tag != 7 { return 22; }
    Holder h = new Holder();
    h.name = "held";
    if h.name != "held" { return 23; }
    h = null; // 全局 string 与 char[N] 字面量初始化
    if G != "global" or len(G) != 6 { return 24; }
    char[8] buf = "abc";
    if buf[2] != 'c' or buf[3] != 0 { return 25; }

    // 拼接结果可安全传回 C
    if puts("str struct ok") < 0 { return 27; }
    return 0;
}


