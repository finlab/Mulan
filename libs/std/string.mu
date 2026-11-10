// mu 标准库：字符串
//
// 这是纯 mu 实现；编译器只负责字面量、s[i]、len、==/!=、for-in 等
// canonical 布局的快速降级。
//
// 字符串是值类型 struct：赋值/传参复制三个字段（p/n/owner），owner 是
// 带 RC 的字节数组，保证底层缓冲的生命周期。

public unit std.string;

public struct string {
private:
    byte* p;
    usize n;
    byte[] owner;

public:
    // 从 NUL 结尾的 C 字符串构造（复制一份，不持有 c）
    string(const char* c) {
        usize count = 0;
        while c[count] != 0 {
            count += 1;
        }
        byte[] buf = new byte[count + 1];
        usize i = 0;
        while i < count {
            buf[i] = c[i] as byte;
            i += 1;
        }
        buf[count] = 0;
        p = &buf[0];
        n = count;
        owner = buf;
    }

    // ---------------------------------------------------------------- 基本信息

    usize length() {
        return n;
    }

    bool empty() {
        return n == 0;
    }

    bool is_empty() {
        return n == 0;
    }

    // 越界返回 0（调试期可改为 panic）
    byte at(usize i) {
        if i >= n {
            return 0;
        }
        return p[i];
    }

    // NUL 结尾视图；字面量与普通字符串都保证末尾有 0
    const char* cstr() {
        return p as const char*;
    }

    // ---------------------------------------------------------------- 切片与拼接

    string sub(usize start, usize count) {
        string r = {};
        r.p = p + start;
        r.n = count;
        r.owner = owner;
        return r;
    }

    string concat(string other) {
        usize total = n + other.n;
        byte[] buf = new byte[total + 1];
        usize i = 0;
        while i < n {
            buf[i] = p[i];
            i += 1;
        }
        usize j = 0;
        while j < other.n {
            buf[n + j] = other.p[j];
            j += 1;
        }
        buf[total] = 0;
        string r = {};
        r.p = &buf[0];
        r.n = total;
        r.owner = buf;
        return r;
    }

    // ---------------------------------------------------------------- 查找与匹配

    // 返回首次出现的字节偏移，找不到返回 -1
    int find(string needle) {
        if needle.n == 0 {
            return 0;
        }
        if needle.n > n {
            return -1;
        }
        usize i = 0;
        while i + needle.n <= n {
            usize j = 0;
            bool same = true;
            while j < needle.n {
                if p[i + j] != needle.p[j] {
                    same = false;
                    break;
                }
                j += 1;
            }
            if same {
                return i as int;
            }
            i += 1;
        }
        return -1;
    }

    bool starts_with(string prefix) {
        if prefix.n > n {
            return false;
        }
        usize i = 0;
        while i < prefix.n {
            if p[i] != prefix.p[i] {
                return false;
            }
            i += 1;
        }
        return true;
    }

    bool ends_with(string suffix) {
        if suffix.n > n {
            return false;
        }
        usize off = n - suffix.n;
        usize i = 0;
        while i < suffix.n {
            if p[off + i] != suffix.p[i] {
                return false;
            }
            i += 1;
        }
        return true;
    }

    // ---------------------------------------------------------------- 修剪

    string trim() {
        usize start = 0;
        while start < n and is_space_byte(p[start]) {
            start += 1;
        }
        usize stop = n;
        while stop > start and is_space_byte(p[stop - 1]) {
            stop -= 1;
        }
        return sub(start, stop - start);
    }

private:
    bool is_space_byte(byte c) {
        return c == ' ' or c == '\t' or c == '\n' or c == '\r';
    }
}

// 运算符只能是自由函数（语言限制），薄封装到成员方法。
public string operator+(string a, string b) {
    return a.concat(b);
}
