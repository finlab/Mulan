// mu 标准库：字符串
//
// 这是纯 mu 实现；编译器只负责字面量、s[i]、len、==/!=、for-in 等
// canonical 布局的快速降级。
unit std.string;

struct string {
    byte* p;
    usize n;
    byte[] owner;

    usize length() { return n; }
    bool empty() { return n == 0; }
    byte at(usize i) { return p[i]; }
}

string str_from_cstr(readonly char* c) {
    usize count = 0;
    while c[count] != 0 { count += 1; }
    byte[] buf = new byte[count + 1];
    usize i = 0;
    while i < count {
        buf[i] = c[i] as byte;
        i += 1;
    }
    buf[count] = 0;
    string s = 0;
    s.p = &buf[0];
    s.n = count;
    s.owner = buf;
    return s;
}

readonly char* str_cstr(string s) {
    return s.p as readonly char*;
}

string str_sub(string s, usize start, usize count) {
    string r = 0;
    r.p = s.p + start;
    r.n = count;
    r.owner = s.owner;
    return r;
}

string str_concat(string a, string b) {
    usize total = a.n + b.n;
    byte[] buf = new byte[total + 1];
    usize i = 0;
    while i < a.n {
        buf[i] = a.p[i];
        i += 1;
    }
    usize j = 0;
    while j < b.n {
        buf[a.n + j] = b.p[j];
        j += 1;
    }
    buf[total] = 0;
    string r = 0;
    r.p = &buf[0];
    r.n = total;
    r.owner = buf;
    return r;
}

string operator+(string a, string b) {
    return str_concat(a, b);
}
